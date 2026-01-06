#pragma once

/*
woomem_os_mmap.h
*/

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

    size_t woomem_os_page_size(void);
    /* OPTIONAL */ void* woomem_os_reserve_memory(size_t size);
    int /* 0 means OK */ woomem_os_commit_memory(void* addr, size_t size);
    int /* 0 means OK */ woomem_os_decommit_memory(void* addr, size_t size);
    int /* 0 means OK */ woomem_os_release_memory(void* addr, size_t size);


#ifdef __cplusplus
}
#endif