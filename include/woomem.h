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

extern uint8_t  woomem_gc_marking_round_counter;
extern bool     woomem_gc_marking_state_flag;
extern size_t   woomem_gc_memory_size_after_last_round_sweep;

typedef enum woomem_Attrib
{
    WOOMEM_ATTRIB_NEED_SWEEP = 1,
    WOOMEM_ATTRIB_AUTO_MARK = 1 << 1,
    WOOMEM_ATTRIB_MARK_CALLBACK = 1 << 2,
    WOOMEM_ATTRIB_FREE_CALLBACK = 1 << 3,

}woomem_Attrib;

typedef void (*woomem_MarkCallback)(void*);
typedef void (*woomem_FreeCallback)(void*);
typedef void (*woomem_GCCallback)(void);

bool woomem_init(
    size_t reserved_chunk_size,
    woomem_GCCallback gc_callback_at_begin,
    woomem_GCCallback gc_callback_at_stop_marking,
    woomem_MarkCallback mark_callback,
    woomem_FreeCallback free_callback);
void woomem_shutdown(void);

void woomem_trigger_gc(bool async);

void* woomem_allocate_begin(size_t size);
void woomem_allocate_end(void* p, int attrib);
void* woomem_reallocate(void* ptr, size_t size);

void* woomem_validate_addr(void* ptr_may_invalid);
void* woomem_validate_addr_head(void* ptr_may_invalid);

void woomem_mark_unit_head(void* ptr_head_may_null);
void woomem_mark_fuzzy_unit(void* ptr_may_invalid_or_null);
void woomem_mark_fuzzy_unit_head(void* ptr_head_may_invalid_null);

void woomem_mark_root_unit_head(void* ptr_head_may_null);
void woomem_mark_root_fuzzy_unit(void* ptr_may_invalid_or_null);
void woomem_mark_root_fuzzy_unit_head(void* ptr_head_may_invalid_null);

#ifdef __cplusplus
}
#endif
