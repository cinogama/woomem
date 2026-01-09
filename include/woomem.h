#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
    void woomem_init(void);
    void woomem_shutdown(void);

    /*
    woomem_GCUnitType
    */
    typedef enum woomem_GCUnitType
    {
        /*
        这一片内存既不是 GC单元，也不用于存储其他 GC 分配的内存，只是普通的内存分配单元。
        */
        WOOMEM_GC_UNIT_TYPE_NORMAL = 0,

        /*
        这一片内存可能包含一个或多个 GC 单元的 `引用`，当标记这片内存时，会自动标记其引用
        的其他内存单元。
        */
        WOOMEM_GC_UNIT_TYPE_AUTO_MARK = 1,

        /*
        这一片内存储存一个 GC 单元，其被标记和回收时，会触发指定的标记/回收回调。同时，
        与 WOOMEM_GC_UNIT_TYPE_AUTO_MARK 类似，这片内存引用的其他内存单元也会被自动标记。
        */
        WOOMEM_GC_UNIT_TYPE_IS_GCUNIT = 2,

    } woomem_GCUnitType;

    /*
    woomem_GCMarkedType
    */
    typedef enum woomem_GCMarkedType
    {
        /*
        内存分配完成后，以及一轮回收后的状态，表示该内存单元未被标记。如果一个非当前轮次分配的内存单元
        在垃圾回收过程中没有被标记，它将在 woomem_end_gc_mark_and_free_all_unmarked 函数中被释放（除非
        对象已经成为老年代对象，并且此次回收并不考虑老年代）。
        */
        WOOMEM_GC_MARKED_UNMARKED = 0,

        /*
        此单元已经被初步标记，但其引用的其他内存单元尚未被标记。在垃圾回收过程中，如果一个内存单元
        被标记为 WOOMEM_GC_MARKED_SELF_MARKED，那么之后应当完全扫描其所有元素，然后调用 woomem_full_mark
        将其标记为 WOOMEM_GC_MARKED_FULL_MARKED。
        */
        WOOMEM_GC_MARKED_SELF_MARKED = 1,

        /*
        此单元已经完全被标记，包括其引用的所有内存单元。
        */
        WOOMEM_GC_MARKED_FULL_MARKED = 2,

        /*
        该内存单元不应被垃圾回收器释放，其引用的其他单元也不需要标记和释放，通常用于特殊用途的内存单元。
        外部GC实现可能在特定情况下将其回落标记为 WOOMEM_GC_MARKED_FULL_MARKED。
        */
        WOOMEM_GC_MARKED_DONOT_RELEASE = 3,

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
    /* OPTIONAL */ void* woomem_alloc_auto_mark(size_t size);
    /* OPTIONAL */ void* woomem_alloc_gcunit(size_t size);

    /*
    重新分配给定的内存单元为新的大小，并返回新的指针。
    */
    /* OPTIONAL */ void* woomem_realloc(void* ptr, size_t new_size);

    /*
    解分配给定的指针，该指针必须是合法的，与 woomem_try_mark_self 不同，分配器不会特地检
    查指定单元的合法性。
    */
    /* OPTIONAL */ void woomem_free(void* ptr);

    /*
    GC接口，外部的GC实现通过此接口尝试检查和初步标记一个可能的地址，如果：
        1）给定的单元不是一个合法的地址（并非有效地址，或者不是当前分配器分配的地址）
        2）已经被标记（无论被标记为何种状态）
        3）属于WOOMEM_GC_MARKED_DONOT_RELEASE
        4）正在执行 MINOR_GC且当前对象是老年代，

    则返回 NULL，否则将该单元标记为 WOOMEM_GC_MARKED_SELF_MARKED 并返回该单元的实际地址。

    如果一个对象即将从新生代晋升为老年代，则 *out_becoming_old 将被设置为 WOOMEM_BOOL_TRUE，
    否则为 WOOMEM_BOOL_FALSE。
    */
    /* OPTIONAL */ void* woomem_try_mark_self(intptr_t maybe_ptr, woomem_Bool* out_becoming_old);

    /*
    GC接口，当外部的GC实现完成对某个单元的完整扫描后，调用此接口将该单元的 m_gc_marked 标记为
    WOOMEM_GC_MARKED_FULL_MARKED。
    */
    void woomem_full_mark(void* ptr);

    /*
    GC接口，宣告一轮新的标记-回收开始。
    更新内部GC轮次计数器，表示一轮新的GC开始。
    如果 is_full_gc 为 TRUE，则表示此次GC为 FULL_GC，否则为 MINOR_GC。
    */
    void woomem_begin_gc_mark(woomem_Bool is_full_gc);

    /*
    GC接口，宣告一轮新的标记-回收结束。
    释放所有未标记的内存单元（除非对象是老年代，且正在执行 MINOR_GC）。
    */
    void woomem_end_gc_mark_and_free_all_unmarked(
        woomem_DestroyFunc  destroy_func,
        void*               userdata);

#ifdef __cplusplus
}
#endif