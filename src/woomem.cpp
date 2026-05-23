#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_page.hpp"
#include "woomem_chunk.hpp"

#include "woomem_global_page_collection.hpp"
#include "woomem_thread_page_collection.hpp"

#include <cassert>

bool woomem_is_gc_in_marking = false;

struct _woomem_GlobalContext
{
    woomem::Chunk m_chunk;

    std::atomic<woomem::PageHead*> m_page_list;

    _woomem_GlobalContext(size_t reserved_chunk_size)
        : m_chunk(reserved_chunk_size)
        , m_page_list{}
    {}

    void add_page_into_chain(woomem::PageHead* page)
    {
        page->m_next_page = m_page_list.load(std::memory_order_relaxed);

        do
        {
        } while (!m_page_list.compare_exchange_weak(
            page->m_next_page,
            page,
            std::memory_order_release,
            std::memory_order_relaxed));
    }
    woomem::PageHead* allocate_new_page()
    {
        return m_chunk.allocate_page();
    }
    woomem::PageHead* allocate_huge_page(size_t size)
    {
        return m_chunk.allocate_huge_page(size);
    }
};
static _woomem_GlobalContext* _s_ctx = nullptr;

bool woomem_init(
    size_t reserved_chunk_size,
    woomem_MarkCallback mark_callback,
    woomem_FreeCallback free_callback)
{
    assert(_s_ctx == nullptr);

    _s_ctx = static_cast<_woomem_GlobalContext*>(malloc(sizeof(_woomem_GlobalContext)));
    if (_s_ctx == nullptr)
        return false;

    (void)new (_s_ctx)_woomem_GlobalContext(reserved_chunk_size);
    if (_s_ctx->m_chunk.is_init_failed())
    {
        woomem_shutdown();
        return false;
    }

    return true;
}
void woomem_shutdown(void)
{
    assert(_s_ctx != nullptr);

    _s_ctx->~_woomem_GlobalContext();
    free(_s_ctx);
}

// ======================================================================

void woomem_trigger_gc(bool async);

void* woomem_validate_addr(void* ptr_may_invalid);
void* woomem_allocate(size_t size, int attrib);
void* woomem_reallocate(void* ptr, size_t size);
void woomem_free(void* ptr);

void woomem_mark_unit_head(void* ptr_head_may_null);
void woomem_mark_fuzzy_unit(void* ptr_may_invalid_or_null);
