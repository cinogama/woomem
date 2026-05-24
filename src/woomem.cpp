#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_page.hpp"
#include "woomem_chunk.hpp"

#include "woomem_global_page_collection.hpp"
#include "woomem_thread_page_collection.hpp"

#include <cassert>
#include <unordered_set>
#include <mutex>

bool woomem_is_gc_in_marking = false;

struct _woomem_ThreadContext;
struct _woomem_GlobalContext
{
    union
    {
        char __keep__;
        struct
        {
            woomem::Chunk m_chunk;
            woomem::GlobalPageCollection m_global_page_collection;
        };
    };
    bool m_globalcontext_alive;
    bool m_globalcontext_inited;

    std::mutex m_thread_entries_mx;
    std::unordered_set<_woomem_ThreadContext*> m_thread_entries;
    std::atomic<woomem::PageHead*> m_all_page_list;

    _woomem_GlobalContext()
        : __keep__{}
        , m_globalcontext_alive(true)
        , m_globalcontext_inited(false)
    {}
    ~_woomem_GlobalContext()
    {
        assert(!m_globalcontext_inited);
        m_globalcontext_alive = false;
    }

    _woomem_GlobalContext(const _woomem_GlobalContext&) = delete;
    _woomem_GlobalContext& operator=(const _woomem_GlobalContext&) = delete;
    _woomem_GlobalContext(_woomem_GlobalContext&&) = delete;
    _woomem_GlobalContext& operator=(_woomem_GlobalContext&&) = delete;

    bool init(size_t reserved_chunk_size)
    {
        assert(!m_globalcontext_inited);

        (void)new (&m_chunk)woomem::Chunk(reserved_chunk_size);
        if (m_chunk.is_init_failed())
        {
            m_chunk.~Chunk();
            return false;
        }
        (void)new (&m_global_page_collection)woomem::GlobalPageCollection(&m_chunk);
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
    }
    void shutdown()
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

    void add_page_into_chain(woomem::PageHead* page)
    {
        page->m_next_page = m_all_page_list.load(std::memory_order_relaxed);

        while (!m_all_page_list.compare_exchange_weak(
            page->m_next_page,
            page,
            std::memory_order_release,
            std::memory_order_relaxed))
            /* Atomic retry */;
    }
    woomem::PageHead* allocate_huge_page(size_t size)
    {
        woomem::PageHead* const huge_page = m_chunk.allocate_huge_page(size);
        if (huge_page != nullptr)
            add_page_into_chain(huge_page);

        return huge_page;
    }
};
static _woomem_GlobalContext _s_ctx;

struct _woomem_ThreadContext
{
    woomem::ThreadPageCollection m_thread_page_collection;

    _woomem_ThreadContext()
        : m_thread_page_collection(
            _s_ctx.m_globalcontext_inited ? &_s_ctx.m_global_page_collection : nullptr)
    {
    }

    _woomem_ThreadContext(const _woomem_ThreadContext&) = delete;
    _woomem_ThreadContext& operator=(const _woomem_ThreadContext&) = delete;
    _woomem_ThreadContext(_woomem_ThreadContext&&) = delete;
    _woomem_ThreadContext& operator=(_woomem_ThreadContext&&) = delete;
};
static thread_local _woomem_ThreadContext _t_ctx;

bool woomem_init(
    size_t reserved_chunk_size,
    woomem_MarkCallback mark_callback,
    woomem_FreeCallback free_callback)
{
    assert(!_s_ctx.m_globalcontext_inited);

    return _s_ctx.init(reserved_chunk_size);
}
void woomem_shutdown(void)
{
    _s_ctx.shutdown();
}

// ======================================================================

void woomem_trigger_gc(bool async);

void* woomem_validate_addr(void* ptr_may_invalid);
void* woomem_allocate(size_t size, int attrib);
void* woomem_reallocate(void* ptr, size_t size);
void woomem_free(void* ptr);

void woomem_mark_unit_head(void* ptr_head_may_null);
void woomem_mark_fuzzy_unit(void* ptr_may_invalid_or_null);
