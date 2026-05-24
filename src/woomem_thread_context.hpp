#pragma once

#include "woomem_thread_page_collection.hpp"

namespace woomem
{
    class ThreadContext
    {
    public:
        ThreadPageCollection m_thread_page_collection;

        ThreadContext();
        ~ThreadContext();

        ThreadContext(const ThreadContext&) = delete;
        ThreadContext& operator=(const ThreadContext&) = delete;
        ThreadContext(ThreadContext&&) = delete;
        ThreadContext& operator=(ThreadContext&&) = delete;
    };

    extern thread_local ThreadContext g_thread_context;
}
