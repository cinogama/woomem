#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_global_context.hpp"
#include "woomem_thread_context.hpp"

#include "woomem_page_unit_alloc.hpp"

#include <cassert>

using namespace woomem;

bool woomem_is_gc_in_marking = false;

bool woomem_init(
    size_t reserved_chunk_size,
    woomem_MarkCallback mark_callback,
    woomem_FreeCallback free_callback)
{
    assert(!g_global_context.m_globalcontext_inited);
    return g_global_context.init(reserved_chunk_size);
}
void woomem_shutdown(void)
{
    g_global_context.shutdown();
}

// ======================================================================

void woomem_trigger_gc(bool async);

void* woomem_allocate_begin(size_t size)
{
    if (size <= MAX_IN_PAGE_UNIT_SIZE)
        return t_thread_context.m_thread_page_collection.pick_unit_in_page(size) + 1;
    
    PageHead* const huge_unit_page = 
        g_global_context.allocate_huge_page(sizeof(PageHead) + sizeof(UnitHead) + size);

    return reinterpret_cast<UnitHead*>(huge_unit_page + 1) + 1;
}
void woomem_allocate_end(void* p, int attrib);
void* woomem_reallocate(void* ptr, size_t size);

void woomem_mark_unit_head(void* ptr_head_may_null);
void woomem_mark_fuzzy_unit(void* ptr_may_invalid_or_null);
