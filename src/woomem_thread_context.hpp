#pragma once

#include "woomem_thread_page_collection.hpp"

namespace woomem
{
    class GCWorker;
    class ThreadContext
    {
    public:
        ThreadPageCollection m_thread_page_collection;
        /* OPTIONAL */ GCWorker* m_gc_marking_context;

        ThreadContext();
        ~ThreadContext();

        ThreadContext(const ThreadContext&) = delete;
        ThreadContext& operator=(const ThreadContext&) = delete;
        ThreadContext(ThreadContext&&) = delete;
        ThreadContext& operator=(ThreadContext&&) = delete;
    };

    extern thread_local ThreadContext t_thread_context;
}
