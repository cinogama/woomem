#include "woomem_os_mmap.h"

#ifndef _WIN32

#   include <sys/mman.h>
#   include <unistd.h>

size_t woomem_os_page_size(void)
{
    return getpagesize();
}
/* OPTIONAL */ void* woomem_os_reserve_memory(size_t size)
{
#   ifdef __EMSCRIPTEN__
    void* result = mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON,
        -1,
        0);
#   else
    void* result = mmap(
        nullptr,
        size,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
#   endif
    return result == MAP_FAILED ? nullptr : result;
}
int /* 0 means OK */ woomem_os_commit_memory(void* addr, size_t size)
{
#   ifdef __EMSCRIPTEN__
    return 0;
#   else
    int result = mprotect(
        addr,
        size,
        PROT_READ | PROT_WRITE);

    return result == 0 ? 0 : errno;
#   endif
}
int /* 0 means OK */ woomem_os_decommit_memory(void* addr, size_t size)
{
#   ifdef __EMSCRIPTEN__
    return 0;
#   else
    int result = mprotect(
        addr,
        size,
        PROT_NONE);
    return result == 0 ? 0 : errno;
#   endif
}
int /* 0 means OK */ woomem_os_release_memory(void* addr, size_t size)
{
    int result = munmap(
        addr,
        size);
    return result == 0 ? 0 : errno;
}

#endif