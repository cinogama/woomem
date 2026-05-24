#pragma once

#include "woomem.h"

#include <thread>
#include <condition_variable>

namespace woomem
{
    class GC
    {
        struct GCWorker
        {
            std::thread m_gc_worker_thread;
            GC* m_gc_ctx;

            GCWorker(GC* gc_ctx);

            GCWorker(const GCWorker&) = delete;
            GCWorker& operator=(const GCWorker&) = delete;
            GCWorker(GCWorker&&) = delete;
            GCWorker& operator=(GCWorker&&) = delete;
        };

        const size_t            m_gc_worker_count;
        woomem_GCCallback       m_gc_callback_at_begin;

        enum class WorkerThresholdState
        {
            PENDING,
        };
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
            woomem_GCCallback begin_callback_for_marking_root);
        ~GC();

    public:
        void launch_worker_and_wait_until_done(
            WorkerThresholdState expected_state);
        void wait_for_worker_launch(
            WorkerThresholdState expected_state);

    public:
        void main_thread_job();
    };

    extern GC* g_gc_ctx;
}
