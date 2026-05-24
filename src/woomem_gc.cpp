#include "woomem_gc.hpp"

namespace woomem
{
    static size_t default_gc_worker_count(void)
    {
        const size_t count = std::thread::hardware_concurrency() / 8;
        return count == 0 ? 1 : count;
    }

    GC::GC(
        size_t worker_count,
        woomem_GCCallback begin_callback_for_marking_root)
        : m_gc_worker_count(worker_count != 0 ? worker_count : default_gc_worker_count())
        , m_gc_callback_at_begin(begin_callback_for_marking_root)
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
    }
    GC::~GC()
    {
        // TODO;
    }

    void GC::launch_worker_and_wait_until_done(
        WorkerThresholdState expected_state)
    {
    }
    void GC::wait_for_worker_launch(
        WorkerThresholdState expected_state)
    {
    }
}