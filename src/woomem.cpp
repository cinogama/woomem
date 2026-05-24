#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_global_context.hpp"
#include "woomem_thread_context.hpp"

#include <cassert>

bool woomem_is_gc_in_marking = false;

bool woomem_init(
    size_t reserved_chunk_size,
    woomem_MarkCallback mark_callback,
    woomem_FreeCallback free_callback)
{
    assert(!woomem::g_global_context.m_globalcontext_inited);
    return woomem::g_global_context.init(reserved_chunk_size);
}
void woomem_shutdown(void)
{
    woomem::g_global_context.shutdown();
}

// ======================================================================

void woomem_trigger_gc(bool async);

void* woomem_validate_addr(void* ptr_may_invalid);
void* woomem_allocate(size_t size, int attrib);
void* woomem_reallocate(void* ptr, size_t size);
void woomem_free(void* ptr);

void woomem_mark_unit_head(void* ptr_head_may_null);
void woomem_mark_fuzzy_unit(void* ptr_may_invalid_or_null);
