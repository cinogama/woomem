#include "woomem.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

//// 简单的销毁回调函数
//static int g_destroy_count = 0;
//
//void test_destroy_func(void* ptr, void* userdata)
//{
//    (void)ptr;
//    (void)userdata;
//    g_destroy_count++;
//}
//
//void test_basic_alloc(void)
//{
//    printf("Testing basic allocation...\n");
//    
//    void* ptr = woomem_alloc(100);
//    assert(ptr != NULL);
//    
//    // 写入数据确保内存可用
//    memset(ptr, 0xAB, 100);
//    
//    printf("  Basic allocation: PASSED\n");
//}
//
//void test_gc_mark_and_free(void)
//{
//    printf("Testing GC mark and free...\n");
//    
//    g_destroy_count = 0;
//    
//    // 分配一些内存
//    void* ptr1 = woomem_alloc(64);
//    void* ptr2 = woomem_alloc(128);
//    void* ptr3 = woomem_alloc(256);
//    
//    assert(ptr1 != NULL);
//    assert(ptr2 != NULL);
//    assert(ptr3 != NULL);
//    
//    // 开始 GC 标记（FULL GC）
//    // 注意：begin_gc_mark 会推进 timing 计数器，所以之前分配的对象不属于"当前轮次"
//    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
//    
//    // 只标记 ptr1 和 ptr3，ptr2 将被回收
//    woomem_Bool marked1 = woomem_try_mark_self(ptr1);
//    woomem_Bool marked3 = woomem_try_mark_self(ptr3);
//    
//    assert(marked1 == WOOMEM_BOOL_TRUE);
//    assert(marked3 == WOOMEM_BOOL_TRUE);
//    
//    // 完成标记
//    woomem_full_mark(ptr1);
//    woomem_full_mark(ptr3);
//    
//    // 结束 GC，ptr2 应该被回收（未标记且不是当前轮次分配）
//    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
//    
//    // ptr2 被回收（1个对象）
//    printf("  First GC cycle: destroy_count = %d\n", g_destroy_count);
//    assert(g_destroy_count == 1);  // 只有 ptr2 被回收
//    
//    // 开始第二轮 GC
//    g_destroy_count = 0;
//    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
//    
//    // 这次只标记 ptr1
//    marked1 = woomem_try_mark_self(ptr1);
//    assert(marked1 == WOOMEM_BOOL_TRUE);
//    woomem_full_mark(ptr1);
//    
//    // 结束 GC，ptr3 应该被回收
//    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
//    
//    printf("  Second GC cycle: destroy_count = %d\n", g_destroy_count);
//    assert(g_destroy_count == 1);  // 只有 ptr3 被回收
//    
//    printf("  GC mark and free: PASSED\n");
//}
//
//void test_double_mark(void)
//{
//    printf("Testing double mark prevention...\n");
//    
//    void* ptr = woomem_alloc(64);
//    assert(ptr != NULL);
//    
//    // 开始新一轮 GC 使之前的分配不是当前轮次
//    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
//    woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
//    
//    // 开始实际测试的 GC 轮次
//    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
//    
//    // 第一次标记应该成功
//    woomem_Bool first_mark = woomem_try_mark_self(ptr);
//    assert(first_mark == WOOMEM_BOOL_TRUE);
//    
//    // 第二次标记应该失败（已经被标记）
//    woomem_Bool second_mark = woomem_try_mark_self(ptr);
//    assert(second_mark == WOOMEM_BOOL_FALSE);
//    
//    woomem_full_mark(ptr);
//    woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
//    
//    printf("  Double mark prevention: PASSED\n");
//}
//
//void test_minor_gc_old_generation(void)
//{
//    printf("Testing minor GC with old generation...\n");
//    
//    g_destroy_count = 0;
//    
//    void* old_obj = woomem_alloc(64);
//    assert(old_obj != NULL);
//    
//    // 让对象变成老年代（经过16轮GC存活）
//    for (int i = 0; i < 16; i++)
//    {
//        woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
//        woomem_Bool marked = woomem_try_mark_self(old_obj);
//        if (marked == WOOMEM_BOOL_TRUE)
//        {
//            woomem_full_mark(old_obj);
//        }
//        woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
//    }
//    
//    // 现在 old_obj 应该是老年代（age = 0）
//    // 执行 MINOR GC，不标记 old_obj
//    woomem_begin_gc_mark(WOOMEM_BOOL_FALSE);  // MINOR GC
//    
//    // 不标记 old_obj
//    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
//    
//    // old_obj 不应该被回收（因为是老年代且是 MINOR GC）
//    printf("  Old generation survival in minor GC: destroy_count = %d\n", g_destroy_count);
//    
//    // 执行 FULL GC，不标记 old_obj
//    g_destroy_count = 0;
//    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);  // FULL GC
//    
//    // 不标记 old_obj
//    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
//    
//    // old_obj 应该被回收（因为是 FULL GC）
//    printf("  Old generation collection in full GC: destroy_count = %d\n", g_destroy_count);
//    assert(g_destroy_count == 1);
//    
//    printf("  Minor GC with old generation: PASSED\n");
//}
//
//void test_alloc_during_gc(void)
//{
//    printf("Testing allocation during GC...\n");
//    
//    g_destroy_count = 0;
//    
//    // 开始 GC
//    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
//    
//    // 在 GC 期间分配对象（当前轮次）
//    void* ptr_during_gc = woomem_alloc(64);
//    assert(ptr_during_gc != NULL);
//    
//    // 不标记这个对象，但它不应该被回收（因为是当前轮次分配且年龄为15）
//    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
//    
//    printf("  Alloc during GC (not marked): destroy_count = %d\n", g_destroy_count);
//    assert(g_destroy_count == 0);  // 当前轮次分配的对象不被回收
//    
//    // 下一轮 GC，如果不标记则应该被回收
//    g_destroy_count = 0;
//    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
//    // 不标记
//    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
//    
//    printf("  Next GC cycle (not marked): destroy_count = %d\n", g_destroy_count);
//    assert(g_destroy_count == 1);  // 现在应该被回收
//    
//    printf("  Allocation during GC: PASSED\n");
//}

int main(int argc, char** argv)
{
    //(void)argc;
    //(void)argv;
    //
    //printf("=== woomem Test Suite ===\n\n");
    //
    //woomem_init();
    //
    //test_basic_alloc();
    //test_gc_mark_and_free();
    //test_double_mark();
    //test_minor_gc_old_generation();
    //test_alloc_during_gc();
    //
    //woomem_shutdown();
    //
    //printf("\n=== All tests passed! ===\n");
    //return 0;
}