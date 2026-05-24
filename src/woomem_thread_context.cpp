#include "woomem_thread_context.hpp"
#include "woomem_global_context.hpp"

#include <mutex>

namespace woomem
{
    ThreadContext::ThreadContext()
        : m_thread_page_collection(
            g_global_context.m_globalcontext_inited ? &g_global_context.m_global_page_collection : nullptr)
    {
        if (g_global_context.m_globalcontext_alive)
        {
            std::lock_guard g(g_global_context.m_thread_entries_mx);
            (void)g_global_context.m_thread_entries.emplace(this);
        }
    }
    ThreadContext::~ThreadContext()
    {
        if (g_global_context.m_globalcontext_alive)
        {
            std::lock_guard g(g_global_context.m_thread_entries_mx);
            (void)g_global_context.m_thread_entries.erase(this);
        }
    }
    thread_local ThreadContext t_thread_context;
}
