#include "woomem_os_mmap.h"

#ifdef _WIN32

#   include <windows.h>

size_t woomem_os_page_size(void)
{
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return (size_t)sysInfo.dwPageSize;
}
/* OPTIONAL */ void* woomem_os_reserve_memory(size_t size)
{
    return VirtualAlloc(
        NULL,
        size,
        MEM_RESERVE,
        PAGE_NOACCESS);
}
int /* 0 means OK */ woomem_os_commit_memory(void* addr, size_t size)
{
    void* result = VirtualAlloc(
        addr,
        size,
        MEM_COMMIT,
        PAGE_READWRITE);
    return result == NULL ? (int)GetLastError() : 0;
}
int /* 0 means OK */ woomem_os_decommit_memory(void* addr, size_t size)
{
    BOOL result = VirtualFree(
        addr,
        size,
        MEM_DECOMMIT);
    return result == FALSE ? (int)GetLastError() : 0;
}
int /* 0 means OK */ woomem_os_release_memory(void* addr, size_t size)
{
    BOOL result = VirtualFree(
        addr,
        0,
        MEM_RELEASE);
    return result == FALSE ? (int)GetLastError() : 0;
}

#endif