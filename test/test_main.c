#include "woomem.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

// 简单的销毁回调函数
static int g_destroy_count = 0;

void test_destroy_func(void* ptr, void* userdata)
{
    (void)ptr;
    (void)userdata;
    g_destroy_count++;
}

void test_basic_alloc(void)
{
    printf("Testing basic allocation...\n");
    
    void* ptr = woomem_alloc(100);
    assert(ptr != NULL);
    
    // 写入数据确保内存可用
    memset(ptr, 0xAB, 100);
    
    woomem_free(ptr);
    
    printf("  Basic allocation: PASSED\n");
}

void test_various_sizes(void)
{
    printf("Testing various allocation sizes...\n");
    
    // 测试各种大小的分配
    size_t sizes[] = {1, 8, 16, 24, 32, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 32768, 65536, 131072};
    void* ptrs[sizeof(sizes) / sizeof(sizes[0])];
    
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        ptrs[i] = woomem_alloc(sizes[i]);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], (int)(i & 0xFF), sizes[i]);
    }
    
    // 释放
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        woomem_free(ptrs[i]);
    }
    
    printf("  Various sizes: PASSED\n");
}

void test_gc_mark_and_free(void)
{
    printf("Testing GC mark and free...\n");
    
    g_destroy_count = 0;
    
    // 分配一些内存
    void* ptr1 = woomem_alloc(64);
    void* ptr2 = woomem_alloc(128);
    void* ptr3 = woomem_alloc(256);
    
    assert(ptr1 != NULL);
    assert(ptr2 != NULL);
    assert(ptr3 != NULL);
    
    // 开始 GC 标记（FULL GC）
    // 注意：begin_gc_mark 会推进 timing 计数器，所以之前分配的对象不属于"当前轮次"
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    
    // 只标记 ptr1 和 ptr3，ptr2 将被回收
    void* marked1 = woomem_try_mark_self((intptr_t)ptr1);
    void* marked3 = woomem_try_mark_self((intptr_t)ptr3);
    
    assert(marked1 != NULL);
    assert(marked3 != NULL);
    
    // 完成标记
    woomem_full_mark(ptr1);
    woomem_full_mark(ptr3);
    
    // 结束 GC，ptr2 应该被回收（未标记且不是当前轮次分配）
    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
    
    // ptr2 被回收（1个对象）
    printf("  First GC cycle: destroy_count = %d\n", g_destroy_count);
    assert(g_destroy_count == 1);  // 只有 ptr2 被回收
    
    // 开始第二轮 GC
    g_destroy_count = 0;
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    
    // 这次只标记 ptr1
    marked1 = woomem_try_mark_self((intptr_t)ptr1);
    assert(marked1 != NULL);
    woomem_full_mark(ptr1);
    
    // 结束 GC，ptr3 应该被回收
    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
    
    printf("  Second GC cycle: destroy_count = %d\n", g_destroy_count);
    assert(g_destroy_count == 1);  // 只有 ptr3 被回收
    
    printf("  GC mark and free: PASSED\n");
}

void test_double_mark(void)
{
    printf("Testing double mark prevention...\n");
    
    void* ptr = woomem_alloc(64);
    assert(ptr != NULL);
    
    // 开始新一轮 GC 使之前的分配不是当前轮次
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
    
    // 重新分配（因为上面的可能被释放了）
    ptr = woomem_alloc(64);
    assert(ptr != NULL);
    
    // 开始实际测试的 GC 轮次
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    
    // 第一次标记应该成功
    void* first_mark = woomem_try_mark_self((intptr_t)ptr);
    assert(first_mark != NULL);
    
    // 第二次标记应该失败（已经被标记）
    void* second_mark = woomem_try_mark_self((intptr_t)ptr);
    assert(second_mark == NULL);
    
    woomem_full_mark(ptr);
    woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
    
    printf("  Double mark prevention: PASSED\n");
}

void test_alloc_during_gc(void)
{
    printf("Testing allocation during GC...\n");
    
    // 先做一轮清理，确保没有遗留对象
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
    
    g_destroy_count = 0;
    
    // 开始 GC
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    
    // 在 GC 期间分配对象（当前轮次）
    void* ptr_during_gc = woomem_alloc(64);
    assert(ptr_during_gc != NULL);
    
    // 不标记这个对象，但它不应该被回收（因为是当前轮次分配且年龄为15）
    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
    
    printf("  Alloc during GC (not marked): destroy_count = %d\n", g_destroy_count);
    assert(g_destroy_count == 0);  // 当前轮次分配的对象不被回收
    
    // 下一轮 GC，如果不标记则应该被回收
    g_destroy_count = 0;
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    // 不标记
    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
    
    printf("  Next GC cycle (not marked): destroy_count = %d\n", g_destroy_count);
    assert(g_destroy_count == 1);  // 现在应该被回收
    
    printf("  Allocation during GC: PASSED\n");
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    
    printf("=== woomem Test Suite ===\n\n");
    
    woomem_init();
    
    test_basic_alloc();
    test_various_sizes();
    test_gc_mark_and_free();
    test_double_mark();
    test_alloc_during_gc();
    
    woomem_shutdown();
    
    printf("\n=== All tests passed! ===\n");
    return 0;
}