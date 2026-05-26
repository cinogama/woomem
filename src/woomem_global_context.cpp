#include "woomem_global_context.hpp"
#include "woomem_thread_context.hpp"

#include <cassert>
#include <new>

namespace woomem
{
    GlobalContext::GlobalContext()
        : m_chunk_storage{}
        , m_gpc_storage{}
        , m_globalcontext_alive(true)
        , m_globalcontext_inited(false)
    {}

    GlobalContext::~GlobalContext()
    {
        if (m_globalcontext_inited)
            // Only for terminate.
            shutdown();

        assert(!m_globalcontext_inited);
        m_globalcontext_alive = false;
    }

    bool GlobalContext::init(size_t reserved_chunk_size)
    {
        assert(!m_globalcontext_inited);

        (void)new (&chunk()) Chunk(reserved_chunk_size);
        if (chunk().is_init_failed())
        {
            chunk().~Chunk();
            return false;
        }
        (void)new (&gpc()) GlobalPageCollection(&chunk());
        m_globalcontext_inited = true;

        do
        {
            std::lock_guard g(m_thread_entries_mx);
            for (auto* thread_ctx : m_thread_entries)
            {
                thread_ctx->m_thread_page_collection.init_manually(
                    &gpc());
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

        gpc().~GlobalPageCollection();
        chunk().~Chunk();

        m_globalcontext_inited = false;
    }

    void GlobalContext::add_new_page_into_chain(PageHead* page)
    {
        assert(page->m_page_just_allocated.load(
            std::memory_order::memory_order_relaxed));

        page->m_page_just_allocated.store(
            false, std::memory_order::memory_order_release);

        add_page_back_to_into_chain(page);
    }
    void GlobalContext::add_page_back_to_into_chain(PageHead* page)
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
        return chunk().allocate_huge_page(size);
    }

    GlobalContext g_global_context;
}
