#include "woomem_os_mmap.h"

#ifndef _WIN32

#   include <sys/mman.h>
#   include <unistd.h>
#   include <errno.h>

size_t woomem_os_page_size(void)
{
    return getpagesize();
}

/* 
 * 优化：直接分配可读写内存，避免 reserve + commit 两次系统调用
 * 在 Linux 上，mmap 的页面是延迟分配的（demand paging），
 * 所以直接 PROT_READ|PROT_WRITE 不会立即消耗物理内存
 */
/* OPTIONAL */ void* woomem_os_reserve_memory(size_t size)
{
    void* result = mmap(
        NULL,
        size,
        PROT_READ | PROT_WRITE,  // 直接可读写，利用 Linux 的 demand paging
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,  // MAP_NORESERVE 避免预留 swap
        -1,
        0);
    return result == MAP_FAILED ? NULL : result;
}

int /* 0 means OK */ woomem_os_commit_memory(void* addr, size_t size)
{
    // 已经在 reserve 时设置了 PROT_READ|PROT_WRITE，无需额外操作
    (void)addr;
    (void)size;
    return 0;
}

int /* 0 means OK */ woomem_os_decommit_memory(void* addr, size_t size)
{
    // 使用 madvise 告诉内核可以回收这些页面
    // MADV_DONTNEED 会立即释放物理页面，但保留虚拟地址映射
#   ifdef MADV_FREE
    // MADV_FREE 更高效（Linux 4.5+），允许内核在需要时回收
    int result = madvise(addr, size, MADV_FREE);
    if (result != 0) {
        // 回退到 MADV_DONTNEED
        result = madvise(addr, size, MADV_DONTNEED);
    }
#   else
    int result = madvise(addr, size, MADV_DONTNEED);
#   endif
    return result == 0 ? 0 : errno;
}

int /* 0 means OK */ woomem_os_release_memory(void* addr, size_t size)
{
    int result = munmap(addr, size);
    return result == 0 ? 0 : errno;
}

#endif