#pragma once

#include "woomem.h"

#include <thread>
#include <condition_variable>

namespace woomem
{
    class GC;

    class GCWorker
    {
        GC* m_gc_ctx;

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
            woomem_GCCallback callback_mark_end);
        ~GC();

    public:
        void launch_worker_and_wait_until_done(
            WorkerThresholdState expected_state);
        void wait_for_worker_launch(
            WorkerThresholdState expected_state);
        void worker_done_and_notify_main_gc_thread();

    public:
        GCWorker* fetch_thread_worker();

    public:
        void main_thread_job();
    };

    extern GC* g_gc_ctx;
}
