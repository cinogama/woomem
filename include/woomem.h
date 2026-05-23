#pragma once

#ifdef __cplusplus
#   include <cstddef>
#   include <cstdint>
extern "C" {
#else
#   include <stddef.h>
#   include <stdint.h>
#   include <stdbool.h>
#endif

extern bool woomem_is_gc_in_marking;

typedef enum woomem_Attrib
{
    WOOMEM_ATTRIB_NEED_SWEEP        = 1,
    WOOMEM_ATTRIB_AUTO_MARK         = 1 << 1,
    WOOMEM_ATTRIB_MARK_CALLBACK     = 1 << 2,
    WOOMEM_ATTRIB_FREE_CALLBACK     = 1 << 3,

}woomem_Attrib;

typedef void woomem_MarkCallback(void*);
typedef void woomem_FreeCallback(void*);

void woomem_init(
    size_t reserved_chunk_size,
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

#ifdef __cplusplus
}
#endif
