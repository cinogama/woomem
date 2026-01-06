#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
    void woomem_init(void);
    void woomem_shutdown(void);

    /*
    woomem_GCMarkedType

    用于标记内存单元的 GC 标记状态，以便垃圾回收器能够识别和处理不同状态的内存单元。
    WOOMEM_GC_MARKED_UNMARKED：
    WOOMEM_GC_MARKED_SELF_MARKED：表示该内存单元已被标记为活动，但其引用的其他内存单元未被标记。
    WOOMEM_GC_MARKED_FULL_MARKED：表示该内存单元及其引用的所有内存单元均已被标记为活动。
    WOOMEM_GC_MARKED_DONOT_RELEASE：表示该内存单元不应被垃圾回收器释放，通常用于特殊用途的内存单元。
                                    此状态会在特殊情况下回落到 WOOMEM_GC_MARKED_FULL_MARKED
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
        */
        WOOMEM_GC_MARKED_DONOT_RELEASE = 3,

    } woomem_GCMarkedType;

    typedef enum woomem_Bool
    {
        WOOMEM_BOOL_FALSE = 0,
        WOOMEM_BOOL_TRUE = 1,

    } woomem_Bool;

    /*
    woomem_MemoryAttribute
    每个分配的内存单元都会携带一个 woomem_MemoryAttribute 属性，用于存储该内存单元的垃圾回收相关信息。
    */
    typedef struct woomem_MemoryAttribute
    {
        /* 
        m_gc_age: 初始值为 15，该单元每存活过一轮回收，此值将减少 1 直到其归零成为老年代对象 
        */
        uint8_t m_gc_age : 4;

        /*
        m_gc_marked: 初始值为 WOOMEM_GC_MARKED_UNMARKED，表示该内存单元的垃圾回收标记状态
        */
        uint8_t m_gc_marked : 2;

        /*
        m_alloc_timing: 记录该内存单元的分配时间点，用于区分不同回收周期内分配的内存单元，初始值由内部
            计数器决定，每次调用 woomem_begin_gc_mark，内部计数器将推进 1；内部计数器将在 0-3 之间循环。
            释放操作始终不考虑当前轮次分配的内存单元（即 m_alloc_timing 等于当前轮次值，且年龄为 15 的
            内存单元）。
        */
        uint8_t m_alloc_timing : 2;

    } woomem_MemoryAttribute;

    typedef void (*woomem_DestroyFunc)(void*, void*);

    /*
    分配指定大小的内存单元，并返回指向该内存单元的指针。
    */
    void* woomem_alloc(size_t size);

    /*
    GC接口，外部的GC实现通过此接口改变 m_gc_marked 标记为 WOOMEM_GC_MARKED_SELF_MARKED；
    如果：1）指定的单元非法，2）已经被标记（无论被标记为何种状态）3）属于WOOMEM_GC_MARKED_DONOT_RELEASE
    4）正在执行 MINOR_GC且当前对象是老年代，则返回 WOOMEM_BOOL_FALSE，否则将该单元标记为 WOOMEM_GC_MARKED_SELF_MARKED
    并返回 WOOMEM_BOOL_TRUE。

    NOTE: 如果考虑使用分代回收，外部 GC 实现应当在老年代对象直接引用新生代对象时，将此新生代对象特殊处理。
    */
    woomem_Bool woomem_try_mark_self(void* ptr);

    /*
    GC接口，当外部的GC实现完成对某个单元的完整扫描后，调用此接口将该单元的 m_gc_marked 标记为
    WOOMEM_GC_MARKED_FULL_MARKED。

    NOTE: 根据设计，完整标记的对象不能引用未标记的对象，外部 GC 实现应当使用检查点机制以确保这一点。
    */
    void woomem_full_mark(void* ptr);

    /*
    GC接口，宣告一轮新的标记-回收开始。
    更新内部计数器，并将当前轮次标记为活动状态。
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