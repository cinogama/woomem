#pragma once

#include "woomem.h"
#include "woomem_mpsc_queue.hpp"

#include <atomic>
#include <vector>
#include <thread>
#include <array>
#include <condition_variable>

namespace woomem
{
    class GC;
    struct UnitHead;
    struct PageHead;

    class GCWorker
    {
        friend class GC;

        GC* m_gc_ctx;

        static constexpr size_t GRAY_QUEUE_CAPACITY = 8192;
        MpscGrayQueue<GRAY_QUEUE_CAPACITY> m_gray_queue;

        std::vector<UnitHead*> m_local_work;
        std::array<UnitHead*, GRAY_QUEUE_CAPACITY> m_drain_buf;

        PageHead* m_sweep_page_list;

        std::thread m_gc_worker_thread;
    public:
        GCWorker(GC* gc_ctx);
        ~GCWorker();

        GCWorker(const GCWorker&) = delete;
        GCWorker& operator=(const GCWorker&) = delete;
        GCWorker(GCWorker&&) = delete;
        GCWorker& operator=(GCWorker&&) = delete;

    public:
        void mark_unit_to_gray(UnitHead* unit_head);
        bool check_and_free_unmarked_unit(UnitHead* unit);
        void sweep_units_in_page(PageHead* page);

    private:
        void process_gray_units();
        void drain_queue_into_local();

    public:
        void worker_thread_job();
    };
    class GC
    {
    public:
        enum class WorkerThresholdState
        {
            PENDING,
            PARALLEL_MARK,
            FINAL_MARK,
            SWEEP,
        };

    private:
        const size_t            m_gc_worker_count;
        std::atomic_size_t      m_gc_assigned_thread_idx;
        woomem_GCCallback       m_gc_callback_at_begin;
        woomem_GCCallback       m_gc_callback_at_stop_marking;
        woomem_GCCallback       m_gc_callback_at_end;
        woomem_MarkCallback     m_user_mark_callback;
        woomem_FreeCallback     m_user_free_callback;

        WorkerThresholdState    m_gc_worker_threshold_launch_state;
        size_t                  m_gc_worker_threshold_finish_counter;
        std::mutex              m_gc_worker_threshold_mx;
        std::condition_variable m_gc_worker_threshold_cv;

        GCWorker*               m_gc_worker_threads;
        std::thread             m_gc_main_thread;

    public:
        GC(const GC&) = delete;
        GC& operator=(const GC&) = delete;
        GC(GC&&) = delete;
        GC& operator=(GC&&) = delete;

        GC(size_t worker_count, 
            woomem_GCCallback callback_for_marking_root,
            woomem_GCCallback callback_stop_marking,
            woomem_GCCallback callback_mark_end,
            woomem_MarkCallback user_mark_callback,
            woomem_FreeCallback user_free_callback);
        ~GC();

    public:
        void launch_worker_and_wait_until_done(
            WorkerThresholdState expected_state);
        void wait_for_worker_launch(
            WorkerThresholdState expected_state);
        void worker_done_and_notify_main_gc_thread();

        void callback_user_mark(void* unit);
        void callback_user_free(void* unit);

    public:
        GCWorker* fetch_thread_worker();

    public:
        void main_thread_job();
    };

    extern GC* g_gc_ctx;
}
