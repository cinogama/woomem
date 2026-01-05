#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
    void woomem_init(void);
    void woomem_shutdown(void);

    typedef enum woomem_GCMarkedType
    {
        WOOMEM_GC_MARKED_UNMARKED = 0,
        WOOMEM_GC_MARKED_SELF_MARKED = 1,
        WOOMEM_GC_MARKED_FULL_MARKED = 2,
        WOOMEM_GC_MARKED_DONOT_RELEASE = 3,

    } woomem_GCMarkedType;

    typedef struct woomem_MemoryAttribute
    {
        uint8_t m_gc_age        : 4;
        uint8_t m_gc_marked     : 2;
        uint8_t m_alloc_timing  : 2;

    } woomem_MemoryAttribute;

    void* woomem_alloc(size_t size);

    /* OPTIONAL */ woomem_MemoryAttribute* woomem_varify_address(void* ptr);

    typedef void (*woomem_DestroyFunc)(void*, void*);
    void woomem_gc_destroy_and_free_all_unmarked(
        woomem_DestroyFunc func, 
        void* user_data);

#ifdef __cplusplus
}
#endif