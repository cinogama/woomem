#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
    typedef void* woomem_UserContext;
    typedef void(*woomem_MarkCallbackFunc)(woomem_UserContext, void*);
    typedef void(*woomem_DestroyCallbackFunc)(woomem_UserContext, void*);
    typedef void(*woomem_RootMarkingFunc)(woomem_UserContext);

    void woomem_init(
        /* OPTIONAL */ woomem_UserContext user_ctx,
        /* OPTIONAL */ woomem_MarkCallbackFunc marker,
        /* OPTIONAL */ woomem_DestroyCallbackFunc destroyer,
        /* OPTIONAL */ woomem_RootMarkingFunc start_marking,
        /* OPTIONAL */ woomem_RootMarkingFunc stop_marking);
    void woomem_shutdown(void);

    /*
    woomem_GCUnitType
    */
    typedef enum woomem_GCUnitTypeMask
    {
        // 如果未被标记，这段内存需要在GC过程中被释放
        WOOMEM_GC_UNIT_TYPE_NEED_SWEEP = 1 << 0,

        // 这段内存如果被标记，需要扫描其内部的所有疑似引用
        WOOMEM_GC_UNIT_TYPE_AUTO_MARK = 1 << 1,

        // 当标记到此单元时，需要调用注册的标记回调函数
        WOOMEM_GC_UNIT_TYPE_HAS_MARKER = 1 << 2,

        // 当释放此单元时，需要调用注册的析构函数
        WOOMEM_GC_UNIT_TYPE_HAS_FINALIZER = 1 << 3,

    } woomem_GCUnitTypeMask;

    /*
    woomem_GCMarkedType
    */
    typedef enum woomem_GCMarkedType
    {
        /*
        此内存单元未被分配
        */
        WOOMEM_GC_MARKED_RELEASED = 0,

        /*
        内存分配完成后，以及一轮回收后的状态，表示该内存单元未被标记。如果一个非当前轮次分配的内存单元
        在垃圾回收过程中没有被标记，它将在 woomem_end_gc_mark_and_free_all_unmarked 函数中被释放（除非
        对象已经成为老年代对象，并且此次回收并不考虑老年代）。
        */
        WOOMEM_GC_MARKED_UNMARKED = 1,

        /*
        此单元已经被初步标记，但其引用的其他内存单元尚未被标记。在垃圾回收过程中，如果一个内存单元
        被标记为 WOOMEM_GC_MARKED_SELF_MARKED，那么之后应当完全扫描其所有元素，然后调用 woomem_full_mark
        将其标记为 WOOMEM_GC_MARKED_FULL_MARKED。
        */
        WOOMEM_GC_MARKED_SELF_MARKED = 2,

        /*
        此单元已经完全被标记，包括其引用的所有内存单元。
        */
        WOOMEM_GC_MARKED_FULL_MARKED = 3,

    } woomem_GCMarkedType;

    typedef enum woomem_Bool
    {
        WOOMEM_BOOL_FALSE = 0,
        WOOMEM_BOOL_TRUE = 1,

    } woomem_Bool;

    typedef void (*woomem_DestroyFunc)(void*, void*);

    /*
    分配指定大小的内存单元，并返回指向该内存单元的指针。
    */
    /* OPTIONAL */ void* woomem_alloc_normal(size_t size);
    /* OPTIONAL */ void* woomem_alloc_attrib(size_t size, int attrib);

    /*
    重新分配给定的内存单元为新的大小，并返回新的指针。
    */
    /* OPTIONAL */ void* woomem_realloc(void* ptr, size_t new_size);

    /*
    解分配给定的指针，该指针必须：

    1）如果 WOOMEM_GC_UNIT_TYPE_NEED_SWEEP，被释放的内存必须仍然可达（即便如此，仍然不推荐手动释放GC对象）
    2）否则，至少依然有效，不能为空指针。

    与 woomem_try_mark_self 不同，分配器不会特地检查指定单元的合法性。
    */
    void woomem_free(void* ptr);

    /*
    检查 GC 是否正在标记阶段，如果是，返回 WOOMEM_BOOL_TRUE
    */
    woomem_Bool woomem_checkpoint(void);

    /*
    使用此方法标记疑似单元的起始地址
    */
    // void woomem_try_mark_unit_head(intptr_t address_may_invalid);

    /*
    使用此方法标记疑似单元
    */
    void woomem_try_mark_unit(intptr_t address_may_invalid);

    void woomem_try_mark_unit_range(
        intptr_t address_may_invalid,
        intptr_t address_ending_place);

    void woomem_delete_barrier(void* addr);

    void woomem_write_barrier_mixed(void** writing_target_unit, void* addr);

#ifdef __cplusplus
}
#endif