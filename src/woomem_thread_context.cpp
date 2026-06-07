#include "woomem_thread_context.hpp"
#include "woomem_global_context.hpp"
#include "woomem_gc.hpp"

#include <mutex>

namespace woomem
{
    ThreadContext::ThreadContext()
        : m_thread_page_collection(
            g_global_context.m_globalcontext_inited
            ? &g_global_context.gpc()
            : nullptr)
        , m_is_gc_worker_context(false)
    {
        if (g_gc_ctx != nullptr)
            m_gc_marking_context = g_gc_ctx->fetch_thread_worker();
        else
            m_gc_marking_context = nullptr;

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
