#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
    void woomem_init(void);
    void woomem_shutdown(void);

    typedef struct woomem_MemoryAttribute
    {
        uint8_t m_gc_age        : 4;
        uint8_t m_gc_marked     : 2;
        uint8_t m_alloc_timing  : 2;

    } woomem_MemoryAttribute;

    void* woomem_alloc(size_t size);
    void woomem_free(void* ptr);
    void* woomem_realloc(void* ptr, size_t new_size);

    /* OPTIONAL */ woomem_MemoryAttribute* woomem_varify_address(void* ptr);

    typedef void (*woomem_WalkUnitFunc)(void*, size_t, woomem_MemoryAttribute*, void*);
    void woomem_gc_walk_all_unit(woomem_WalkUnitFunc walk_callback, void* userdata);

#ifdef __cplusplus
}
#endif