#include <chrono>
#include <thread>
#include <mutex>
#include <cstdlib>

#include "woomem_gc.hpp"
#include "woomem_global_context.hpp"
#include "woomem_thread_context.hpp"
#include "woomem_page_unit_alloc.hpp"
#include "woomem_lock.hpp"
#include "woomem_rwlock.hpp"

uint8_t woomem_gc_marking_round_counter = 0;
bool woomem_gc_marking_state_flag = false;
size_t woomem_gc_memory_size_after_last_round_sweep = 0;

namespace woomem
{
    GC* g_gc_ctx = nullptr;

    static size_t default_gc_worker_count(void)
    {
        const size_t count = std::thread::hardware_concurrency() / 8;
        return count == 0 ? 1 : count;
    }

    GC::GC(
        size_t worker_count,
        woomem_GCCallback callback_for_marking_root,
        woomem_GCCallback callback_stop_marking,
        woomem_MarkCallback user_mark_callback,
        woomem_FreeCallback user_free_callback,
        woomem_GCMainThreadEntryCallback main_entry_callback,
        woomem_GCWorkerThreadEntryCallback worker_entry_callback)
        : m_gc_worker_count(worker_count != 0 ? worker_count : default_gc_worker_count())
        , m_gc_assigned_thread_idx{}
        , m_shutdown{ false }
        , m_worker_shutdown{ false }
        , m_gc_callback_at_begin(callback_for_marking_root)
        , m_gc_callback_at_stop_marking(callback_stop_marking)
        , m_user_mark_callback(user_mark_callback)
        , m_user_free_callback(user_free_callback)
        , m_main_entry_callback(main_entry_callback)
        , m_worker_entry_callback(worker_entry_callback)
        , m_gc_worker_threshold_launch_state(WorkerThresholdState::PENDING)
        , m_gc_worker_threshold_finish_counter(0)
        , m_force_trigger_gc{ false }
        , m_gc_cycle_count{ 0 }
        , m_new_allocated_size_since_last_gc{ 0 }
    {
        m_gc_worker_threads = (GCWorker*)malloc(m_gc_worker_count * sizeof(GCWorker));
        if (m_gc_worker_threads == nullptr)
            abort();

        // Pre pare for worker threads.
        for (size_t i = 0; i < m_gc_worker_count; ++i)
            (void)new (&m_gc_worker_threads[i]) GCWorker(this);

        m_gc_main_thread = std::thread(&GC::main_thread_job, this);
        do
        {
            std::lock_guard g(g_global_context.m_thread_entries_mx);
            for (ThreadContext* thread_entry : g_global_context.m_thread_entries)
            {
                if (!thread_entry->m_is_gc_worker_context)
                    thread_entry->m_gc_marking_context = fetch_thread_worker();
            }
        } while (0);
    }
    GC::~GC()
    {
        m_shutdown.store(true, std::memory_order::memory_order_release);
        m_gc_main_thread.join();

        do
        {
            std::lock_guard g(g_global_context.m_thread_entries_mx);
            for (ThreadContext* thread_entry : g_global_context.m_thread_entries)
                thread_entry->m_gc_marking_context = nullptr;
        } while (0);

        m_worker_shutdown.store(true, std::memory_order::memory_order_release);
        for (size_t i = 0; i < m_gc_worker_count; ++i)
            m_gc_worker_threads[i].~GCWorker();

        free(m_gc_worker_threads);
    }

    void GC::launch_worker_and_wait_until_done(
        WorkerThresholdState expected_state)
    {
        std::unique_lock ug(m_gc_worker_threshold_mx);

        m_gc_worker_threshold_finish_counter = 0;
        m_gc_worker_threshold_launch_state = expected_state;

        m_gc_worker_threshold_cv.notify_all();
        m_gc_worker_threshold_cv.wait(
            ug,
            [this]()
            {
                return m_gc_worker_count == m_gc_worker_threshold_finish_counter;
            });
    }
    void GC::wait_for_worker_launch(
        WorkerThresholdState expected_state)
    {
        std::unique_lock ug(m_gc_worker_threshold_mx);
        m_gc_worker_threshold_cv.wait(
            ug,
            [this, expected_state]()
            {
                return expected_state == m_gc_worker_threshold_launch_state;
            });
    }
    bool GC::wait_for_worker_launch_or_shutdown(
        WorkerThresholdState expected_state)
    {
        std::unique_lock ug(m_gc_worker_threshold_mx);
        m_gc_worker_threshold_cv.wait(
            ug,
            [this, expected_state]()
            {
                return expected_state == m_gc_worker_threshold_launch_state
                    || m_worker_shutdown.load(std::memory_order_acquire);
            });
        return !m_worker_shutdown.load(std::memory_order_acquire);
    }
    void GC::signal_worker_shutdown()
    {
        {
            std::lock_guard g(m_gc_worker_threshold_mx);
        }
        m_gc_worker_threshold_cv.notify_all();
    }
    void GC::worker_done_and_notify_main_gc_thread()
    {
        std::lock_guard g(m_gc_worker_threshold_mx);

        if (++m_gc_worker_threshold_finish_counter == m_gc_worker_count)
            m_gc_worker_threshold_cv.notify_all();
    }
    void GC::callback_worker_entry()
    {
        m_worker_entry_callback();
    }
    void GC::callback_user_mark(void* unit)
    {
        m_user_mark_callback(unit);
    }
    void GC::callback_user_free(void* unit)
    {
        m_user_free_callback(unit);
    }
    void GC::mark_root_unit_to_gray(UnitHead* unit_head)
    {
        uint8_t expected = UnitLife::UNMARKED;
        if (unit_head->m_life.compare_exchange_strong(
            expected,
            UnitLife::SELF_MARKED,
            std::memory_order::memory_order_release,
            std::memory_order::memory_order_relaxed))
        {
            assert(woomem_gc_marking_state_flag);

            const size_t assigned_worker_id =
                m_gc_assigned_thread_idx.fetch_add(
                    1, std::memory_order::memory_order_relaxed);

            auto& worker = m_gc_worker_threads[assigned_worker_id % m_gc_worker_count];

            std::lock_guard g(worker.m_local_work_spin_for_root);
            worker.m_local_work.push_back(unit_head);
        }
    }
    GCWorker* GC::fetch_thread_worker()
    {
        const size_t assigned_worker_id =
            m_gc_assigned_thread_idx.fetch_add(
                1, std::memory_order::memory_order_relaxed);

        return &m_gc_worker_threads[assigned_worker_id % m_gc_worker_count];
    }
    void GC::trigger_gc(bool async)
    {
        if (async)
        {
            m_force_trigger_gc.store(true, std::memory_order_release);
            m_trigger_cv.notify_one();
        }
        else
        {
            const size_t prev_count = m_gc_cycle_count.load(std::memory_order_acquire);
            m_force_trigger_gc.store(true, std::memory_order_release);
            m_trigger_cv.notify_one();

            std::unique_lock ug(m_trigger_mx);
            m_trigger_cv.wait(ug, [this, prev_count]()
                {
                    return m_gc_cycle_count.load(std::memory_order_acquire) > prev_count
                        || m_shutdown.load(std::memory_order_acquire);
                });
        }
    }

    void GC::register_root_unit_head(UnitHead* unit_head)
    {
        m_root_gc_unit_set_mx.lock();
        m_root_gc_unit_set.insert(unit_head);
        m_root_gc_unit_set_mx.unlock();
    }

    void GC::unregister_root_unit_head(UnitHead* unit_head)
    {
        m_root_gc_unit_set_mx.lock();
        m_root_gc_unit_set.erase(unit_head);
        m_root_gc_unit_set_mx.unlock();
    }

    void GC::main_thread_job()
    {
        m_main_entry_callback();

        using namespace std;
        do
        {
            // Step 0: Wait with ratio-based early triggering.
            {
                static constexpr size_t GC_TRIGGER_NEW_ALLOC_RATIO_NUM = 1;
                static constexpr size_t GC_TRIGGER_NEW_ALLOC_RATIO_DEN = 3;
                static constexpr size_t GC_TRIGGER_MIN_EDGE = 1024 * 1024;

                const auto cycle_start = chrono::steady_clock::now();
                while (true)
                {
                    {
                        std::unique_lock ug(m_trigger_mx);
                        m_trigger_cv.wait_for(ug, 0.1s, [this]()
                            {
                                return m_force_trigger_gc.load(std::memory_order_relaxed)
                                    || m_shutdown.load(std::memory_order_acquire);
                            });
                    }

                    if (m_shutdown.load(std::memory_order_acquire))
                        return;

                    const auto elapsed = chrono::steady_clock::now() - cycle_start;
                    if (elapsed >= 10s)
                        break;

                    if (m_force_trigger_gc.load(std::memory_order_relaxed))
                        break;

                    const size_t alive =
                        std::max(GC_TRIGGER_MIN_EDGE, woomem_gc_memory_size_after_last_round_sweep);

                    const size_t new_alloc =
                        m_new_allocated_size_since_last_gc.load(
                            std::memory_order_relaxed);

                    if (new_alloc * GC_TRIGGER_NEW_ALLOC_RATIO_DEN
                        >= alive * GC_TRIGGER_NEW_ALLOC_RATIO_NUM)
                        break;
                }
            }
            m_force_trigger_gc.store(false, std::memory_order_relaxed);
            m_new_allocated_size_since_last_gc.store(0, std::memory_order_relaxed);

            // Step 1: 更新 GC 轮次和 GC 状态
            ++woomem_gc_marking_round_counter;
            woomem_gc_marking_state_flag = true;

            // Step 2: 触发 GC 起始回调，此阶段完成线程同步和根对象标记
            m_gc_callback_at_begin();

            m_root_gc_unit_set_mx.lock();
            {
                for (UnitHead* const root_unit : m_root_gc_unit_set)
                    mark_root_unit_to_gray(root_unit);
            }
            m_root_gc_unit_set_mx.unlock();

            // Step 3: 根对象标记完成，收集，开始并行标记
            launch_worker_and_wait_until_done(WorkerThresholdState::PARALLEL_MARK);

            // Step 4: 首轮标记结束回调，此阶段通知正在运行的其他线程不要继续标记
            woomem_gc_marking_state_flag = false;
            m_gc_callback_at_stop_marking();

            // Step 5: 收尾标记
            launch_worker_and_wait_until_done(WorkerThresholdState::FINAL_MARK);

            // Step 6: 从全局链表中提取所有 Page，均匀分发给每个 GCWorker
            {
                PageHead* all_pages = g_global_context.m_all_page_list.exchange(
                    nullptr, std::memory_order::memory_order_acq_rel);

                if (all_pages != nullptr)
                {
                    size_t total_pages = 0;
                    for (PageHead* p = all_pages; p != nullptr; p = p->m_next_page)
                        ++total_pages;

                    const size_t base_count = total_pages / m_gc_worker_count;
                    const size_t remainder = total_pages % m_gc_worker_count;

                    PageHead* current = all_pages;
                    for (size_t i = 0; i < m_gc_worker_count; ++i)
                    {
                        const size_t n = base_count + (i < remainder ? 1 : 0);

                        if (n == 0)
                        {
                            m_gc_worker_threads[i].m_sweep_page_list = nullptr;
                            continue;
                        }

                        m_gc_worker_threads[i].m_sweep_page_list = current;

                        PageHead* prev = nullptr;
                        for (size_t j = 0; j < n; ++j)
                        {
                            prev = current;
                            current = current->m_next_page;
                        }
                        prev->m_next_page = nullptr;
                    }
                }
            }
            launch_worker_and_wait_until_done(WorkerThresholdState::SWEEP);

            // Step 7: 统计存活内存单元大小
            size_t total_alive_memory_size = 0;
            for (size_t i = 0; i < m_gc_worker_count; ++i)
            {
                total_alive_memory_size +=
                    m_gc_worker_threads[i].m_alive_memory_size_counter;
            }
            woomem_gc_memory_size_after_last_round_sweep = total_alive_memory_size;

            g_global_context.gpc().remove_marked_run_out_pages();

            m_gc_worker_threshold_launch_state = WorkerThresholdState::PENDING;

            m_gc_cycle_count.fetch_add(1, std::memory_order_release);
            m_trigger_cv.notify_all();
        } while (1);
    }

    GCWorker::GCWorker(GC* gc_ctx)
        : m_gc_ctx(gc_ctx)
        , m_sweep_page_list(nullptr)
    {
        m_local_work.reserve(GRAY_QUEUE_CAPACITY);
        m_gc_worker_thread = std::thread(&GCWorker::worker_thread_job, this);
    }
    GCWorker::~GCWorker()
    {
        m_gc_ctx->signal_worker_shutdown();
        m_gc_worker_thread.join();
    }
    void GCWorker::mark_unit_to_gray(
        UnitHead* unit_head)
    {
        uint8_t expected = UnitLife::UNMARKED;
        if (unit_head->m_life.compare_exchange_strong(
            expected,
            UnitLife::SELF_MARKED,
            std::memory_order::memory_order_release,
            std::memory_order::memory_order_relaxed))
        {
            if (std::this_thread::get_id() == m_gc_worker_thread.get_id())
                m_local_work.push_back(unit_head);
            else
            {
                while (!m_gray_queue.try_enqueue(unit_head))
                {
                    /*
                        如果 Worker 尚未启动（m_is_draining == false），而此时灰度队
                        列已满，那么继续自旋等待队列空闲将会导致死锁：

                        1. GC 主线程在 Step 2 等待 VM 响应 GC_CHECK
                        2. VM 线程在此处自旋（队列满，无人消费）
                        3. Worker 线程等待 PARALLEL_MARK 信号（Step 3 尚未到达）

                        解决办法：直接推入 m_local_work（通过 m_local_work_spin_for_root）。

                        注意与 Worker 的同步：Worker 在进入 process_gray_units 时，会
                        持有同一把自旋锁将 m_is_draining 置为 true。此处持锁后二次
                        检查 m_is_draining：若发现 Worker 已开始消费，则放弃直接推送，
                        继续尝试 enqueue（此时 Worker 正在 drain，队列很快会有空位）。
                    */
                    if (!m_is_draining.load(std::memory_order_acquire))
                    {
                        std::lock_guard g(m_local_work_spin_for_root);
                        if (!m_is_draining.load(std::memory_order_relaxed))
                        {
                            m_local_work.push_back(unit_head);
                            break;
                        }
                    }
                }
            }
        }
    }
    bool GCWorker::check_and_free_unmarked_unit(UnitHead* unit, PageHead* page_may_null)
    {
        const uint8_t life = unit->m_life.load(std::memory_order::memory_order_relaxed);
        switch (life)
        {
        case UnitLife::RELEASED:
            return false;
        case UnitLife::PENDING:
            return true;
        case UnitLife::UNMARKED:
            if ((unit->m_age != 15 || unit->m_timing != woomem_gc_marking_round_counter)
                && 0 != (unit->m_attribute & WOOMEM_ATTRIB_NEED_SWEEP))
            {
                // Drop it.
                if (unit->m_attribute & WOOMEM_ATTRIB_FREE_CALLBACK)
                    m_gc_ctx->callback_user_free(unit + 1);

                unit->m_life.store(
                    UnitLife::RELEASED,
                    std::memory_order::memory_order_relaxed);

                if (page_may_null != nullptr)
                    drop_freed_unit_into_page(page_may_null, unit);

                return false;
            }
            /* fallthrough */
            [[fallthrough]];
        default:
            assert(life != UnitLife::SELF_MARKED);
            if (unit->m_age != 0)
                --unit->m_age;

            unit->m_life.store(
                UnitLife::UNMARKED,
                std::memory_order::memory_order_relaxed);
        }

        return true;
    }
    void GCWorker::sweep_units_in_page(PageHead* page)
    {
        bool drop_page = false;
        assert(!page->m_page_just_allocated.load(std::memory_order::memory_order_relaxed));

        if (page->m_page_count_if_huge == 0)
        {
            PageUnitAlloc* const page_alloc_head =
                reinterpret_cast<PageUnitAlloc*>(page + 1);

            const size_t unit_size_with_head =
                page_alloc_head->m_unit_size_in_page + sizeof(UnitHead);

            char* unit_storage =
                reinterpret_cast<char*>(page_alloc_head + 1);

            const size_t unit_count =
                (PageHead::NORMAL_PAGE_SIZE - sizeof(PageHead) - sizeof(PageUnitAlloc)) / unit_size_with_head;

            bool has_survivor = false, has_free_space = false;

            const bool current_running_out =
                page_alloc_head->m_run_out.load(std::memory_order_acquire);

            for (size_t i = 0; i < unit_count; ++i)
            {
                UnitHead* unit =
                    reinterpret_cast<UnitHead*>(unit_storage + i * unit_size_with_head);

                if (unit->m_life.load(std::memory_order::memory_order_relaxed)
                    == UnitLife::RELEASED)
                    continue;

                if (check_and_free_unmarked_unit(unit, page))
                {
                    has_survivor = true;
                    m_alive_memory_size_counter += unit_size_with_head;
                }
                else
                    has_free_space = true;
            }

            if (current_running_out && has_free_space)
            {
                if (!has_survivor)
                    drop_page = true;
                else
                {
                    page_alloc_head->m_run_out.store(
                        0, std::memory_order::memory_order_relaxed);

                    g_global_context.gpc().return_page(
                        page, eval_group_by_small_unit_size(
                            page_alloc_head->m_unit_size_in_page));
                }
            }

            if (has_survivor)
            {
                m_alive_memory_size_counter +=
                    sizeof(PageHead) + sizeof(PageUnitAlloc);
            }
            else
            {
                if (!current_running_out)
                    page_alloc_head->m_mark_as_run_out_in_global_pool = true;
            }
        }
        else
        {
            // Is huge unit.
            if (!check_and_free_unmarked_unit(reinterpret_cast<UnitHead*>(page + 1), nullptr))
                drop_page = true;
            else
            {
                m_alive_memory_size_counter +=
                    page->m_page_count_if_huge * PageHead::NORMAL_PAGE_SIZE;
            }
        }

        if (drop_page)
            // Drop this page.
            g_global_context.chunk().free_page(page);
        else
            // Re-join the page into list.
            g_global_context.add_page_back_to_into_chain(page);
    }
    void GCWorker::drain_queue_into_local()
    {
        const size_t count = m_gray_queue.drain(
            m_drain_buf.data(), m_drain_buf.size());

        if (count != 0)
        {
            m_local_work.insert(
                m_local_work.end(),
                m_drain_buf.begin(),
                m_drain_buf.begin() + count);
        }
    }
    void GCWorker::process_gray_units()
    {
        /*
            与 mark_unit_to_gray 中的外部线程建立同步：
            外部线程在持锁后二次检查 m_is_draining，若为 false 则推入
            m_local_work。此处持同一把锁将 m_is_draining 置为 true，
            保证 Worker 开始消费 m_local_work 之前，外部线程的推送已
            经完成。
        */
        {
            std::lock_guard g(m_local_work_spin_for_root);
            m_is_draining.store(true, std::memory_order_release);
        }

        while (true)
        {
            if (m_local_work.empty())
                // NOTE: `drain_queue_into_local` contains a acquire order.
                //      So, we can sure the `m_life` of the unit to full mark
                //      is `SELF_MARKED` we can read.
                drain_queue_into_local();
            if (m_local_work.empty())
            {
                m_is_draining.store(false, std::memory_order_release);
                return;
            }

            UnitHead* const unit = m_local_work.back();
            m_local_work.pop_back();

            assert(SELF_MARKED == unit->m_life.load(
                std::memory_order::memory_order_relaxed));

            if (unit->m_attribute & WOOMEM_ATTRIB_MARK_CALLBACK)
            {
                m_gc_ctx->callback_user_mark(unit + 1);
            }
            if (unit->m_attribute & WOOMEM_ATTRIB_AUTO_MARK)
            {
                const size_t auto_mark_step =
                    unit->get_unit_available_size() / sizeof(void*);

                void** const p = reinterpret_cast<void**>(unit + 1);
                for (size_t i = 0; i < auto_mark_step; ++i)
                    woomem_mark_fuzzy_unit(p[i]);
            }

            // Ok mark finished.
            unit->m_life.store(
                UnitLife::FULL_MARKED,
                std::memory_order::memory_order_release);
        }
    }
    void GCWorker::worker_thread_job()
    {
        // Update `m_gc_marking_context` as this.
        t_thread_context.m_is_gc_worker_context = true;
        t_thread_context.m_gc_marking_context = this;

        m_gc_ctx->callback_worker_entry();

        do
        {
            if (!m_gc_ctx->wait_for_worker_launch_or_shutdown(
                GC::WorkerThresholdState::PARALLEL_MARK))
            {
                return;
            }
            else
                process_gray_units();

            m_gc_ctx->worker_done_and_notify_main_gc_thread();
            m_gc_ctx->wait_for_worker_launch(GC::WorkerThresholdState::FINAL_MARK);
            {
                process_gray_units();
            }
            m_gc_ctx->worker_done_and_notify_main_gc_thread();
            m_gc_ctx->wait_for_worker_launch(GC::WorkerThresholdState::SWEEP);
            {
                m_alive_memory_size_counter = 0;

                for (PageHead* page = m_sweep_page_list; page != nullptr;)
                {
                    PageHead* const next_page = page->m_next_page;
                    sweep_units_in_page(page);
                    page = next_page;
                }
            }
            m_gc_ctx->worker_done_and_notify_main_gc_thread();

        } while (1);
    }
}