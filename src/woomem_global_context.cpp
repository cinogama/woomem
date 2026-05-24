#include "woomem_global_context.hpp"
#include "woomem_thread_context.hpp"

#include <cassert>
#include <new>

namespace woomem
{
    GlobalContext::GlobalContext()
        : __keep__{}
        , m_globalcontext_alive(true)
        , m_globalcontext_inited(false)
    {}

    GlobalContext::~GlobalContext()
    {
        assert(!m_globalcontext_inited);
        m_globalcontext_alive = false;
    }

    bool GlobalContext::init(size_t reserved_chunk_size)
    {
        assert(!m_globalcontext_inited);

        (void)new (&m_chunk) Chunk(reserved_chunk_size);
        if (m_chunk.is_init_failed())
        {
            m_chunk.~Chunk();
            return false;
        }
        (void)new (&m_global_page_collection) GlobalPageCollection(&m_chunk);
        m_globalcontext_inited = true;

        do
        {
            std::lock_guard g(m_thread_entries_mx);
            for (auto* thread_ctx : m_thread_entries)
            {
                thread_ctx->m_thread_page_collection.init_manually(
                    &m_global_page_collection);
            }
        } while (0);

        return true;
    }

    void GlobalContext::shutdown()
    {
        assert(m_globalcontext_inited);

        do
        {
            std::lock_guard g(m_thread_entries_mx);
            for (auto* thread_ctx : m_thread_entries)
            {
                thread_ctx->m_thread_page_collection.shutdown_manually();
            }
        } while (0);

        m_global_page_collection.~GlobalPageCollection();
        m_chunk.~Chunk();

        m_globalcontext_inited = false;
    }

    void GlobalContext::add_page_into_chain(PageHead* page)
    {
        page->m_next_page = m_all_page_list.load(std::memory_order_relaxed);

        while (!m_all_page_list.compare_exchange_weak(
            page->m_next_page,
            page,
            std::memory_order_release,
            std::memory_order_relaxed))
            /* Atomic retry */;
    }

    PageHead* GlobalContext::allocate_huge_page(size_t size)
    {
        return m_chunk.allocate_huge_page(size);
    }

    GlobalContext g_global_context;
}
