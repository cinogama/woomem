#include <chrono>
#include <thread>
#include <mutex>

#include "woomem_gc.hpp"
#include "woomem_global_context.hpp"
#include "woomem_thread_context.hpp"

uint8_t woomem_gc_marking_round_counter = 0;
bool woomem_gc_marking_state_flag = false;

namespace woomem
{
    static size_t default_gc_worker_count(void)
    {
        const size_t count = std::thread::hardware_concurrency() / 8;
        return count == 0 ? 1 : count;
    }

    GC::GC(
        size_t worker_count,
        woomem_GCCallback callback_for_marking_root,
        woomem_GCCallback callback_stop_marking,
        woomem_GCCallback callback_mark_end)
        : m_gc_worker_count(worker_count != 0 ? worker_count : default_gc_worker_count())
        , m_gc_assigned_thread_idx{}
        , m_gc_callback_at_begin(callback_for_marking_root)
        , m_gc_callback_at_stop_marking(callback_stop_marking)
        , m_gc_callback_at_end(callback_mark_end)
        , m_gc_worker_threshold_launch_state(WorkerThresholdState::PENDING)
        , m_gc_worker_threshold_finish_counter(0)
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
                thread_entry->m_gc_marking_context = fetch_thread_worker();
        } while (0);
    }
    GC::~GC()
    {
        // TODO: Stop main & worker threads here.
        m_gc_main_thread.join();

        do
        {
            std::lock_guard g(g_global_context.m_thread_entries_mx);
            for (ThreadContext* thread_entry : g_global_context.m_thread_entries)
                thread_entry = nullptr;
        } while (0);

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
    void GC::worker_done_and_notify_main_gc_thread()
    {
        std::lock_guard g(m_gc_worker_threshold_mx);

        if (++m_gc_worker_threshold_finish_counter == m_gc_worker_count)
            m_gc_worker_threshold_cv.notify_all();
    }
    GCWorker* GC::fetch_thread_worker()
    {
        const size_t assigned_worker_id =
            m_gc_assigned_thread_idx.fetch_add(
                1, std::memory_order::memory_order_relaxed);

        return &m_gc_worker_threads[assigned_worker_id % m_gc_worker_count];
    }
    void GC::main_thread_job()
    {
        using namespace std;
        do
        {
            // Step 0: Wait.
            // TODO: 使用更有价值的GC触发策略
            std::this_thread::sleep_for(10s);

            // Step 1: 更新 GC 轮次和 GC 状态
            ++woomem_gc_marking_round_counter;
            woomem_gc_marking_state_flag = true;

            // Step 2: 触发 GC 起始回调，此阶段完成线程同步和根对象标记
            m_gc_callback_at_begin();

            // Step 3: 根对象标记完成，收集，开始并行标记
            launch_worker_and_wait_until_done(WorkerThresholdState::PARALLEL_MARK);

            // Step 4: 首轮标记结束回调，此阶段通知正在运行的其他线程不要继续标记
            m_gc_callback_at_stop_marking();

            // Step 5: 收尾标记
            launch_worker_and_wait_until_done(WorkerThresholdState::FINAL_MARK);

            // Step 6: GC 标记正式结束，最终回调
            m_gc_callback_at_end();

            // Step 7: 遍历所有 Page，回收未标记的单元
            launch_worker_and_wait_until_done(WorkerThresholdState::SWEEP);

            m_gc_worker_threshold_launch_state = WorkerThresholdState::PENDING;
        } while (1);
    }

    GCWorker::GCWorker(GC* gc_ctx)
        : m_gc_ctx(gc_ctx)
    {
        m_gc_worker_thread = std::thread(&GCWorker::worker_thread_job, this);
    }
    GCWorker::~GCWorker()
    {
        // TODO: Stop worker thread here.
        m_gc_worker_thread.join();
    }
    void GCWorker::worker_thread_job()
    {
        do
        {
            m_gc_ctx->wait_for_worker_launch(GC::WorkerThresholdState::PARALLEL_MARK);
            {
            }
            m_gc_ctx->worker_done_and_notify_main_gc_thread();
            m_gc_ctx->wait_for_worker_launch(GC::WorkerThresholdState::FINAL_MARK);
            {
            }
            m_gc_ctx->worker_done_and_notify_main_gc_thread();
            m_gc_ctx->wait_for_worker_launch(GC::WorkerThresholdState::SWEEP);
            {
            }
            m_gc_ctx->worker_done_and_notify_main_gc_thread();

        } while (1);
    }
}