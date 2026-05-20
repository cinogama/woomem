#include "woomem.h"
#include "woomem_os_mmap.h"

bool woomem_is_gc_in_marking = false;

void woomem_init(
    woomem_MarkCallback mark_callback,
    woomem_FreeCallback free_callback);
void woomem_shutdown(void);

void woomem_trigger_gc(bool async);

void* woomem_validate_addr(void* ptr_may_invalid);
void* woomem_allocate(size_t size, int attrib);
void* woomem_reallocate(void* ptr, size_t size);
void woomem_free(void* ptr);

void woomem_mark_unit_head(void* ptr_head_may_null);
void woomem_mark_fuzzy_unit(void* ptr_may_invalid_or_null);
