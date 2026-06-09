#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_global_context.hpp"
#include "woomem_thread_context.hpp"

#include "woomem_page_unit_alloc.hpp"
#include "woomem_gc.hpp"

#include <cassert>
#include <cstring>
#include <cstdlib>

using namespace woomem;

bool woomem_init(
    size_t reserved_chunk_size,
    woomem_GCCallback gc_callback_at_begin,
    woomem_GCCallback gc_callback_at_stop_marking,
    woomem_MarkCallback mark_callback,
    woomem_FreeCallback free_callback,
    woomem_GCMainThreadEntryCallback main_entry_callback,
    woomem_GCWorkerThreadEntryCallback worker_entry_callback)
{
    assert(!g_global_context.m_globalcontext_inited && g_gc_ctx == nullptr);
    if (g_global_context.init(reserved_chunk_size))
    {
        g_gc_ctx = reinterpret_cast<GC*>(malloc(sizeof(GC)));
        if (g_gc_ctx != nullptr)
        {
            (void)new (g_gc_ctx)GC(
                0, 
                gc_callback_at_begin, 
                gc_callback_at_stop_marking, 
                mark_callback, 
                free_callback,
                main_entry_callback,
                worker_entry_callback);
            return true;
        }
        g_global_context.shutdown();
    }
    return false;
}
void woomem_shutdown(void)
{
    g_gc_ctx->~GC();

    g_global_context.shutdown();
    free(g_gc_ctx);

    g_gc_ctx = nullptr;
}

// ======================================================================

void woomem_trigger_gc(bool async)
{
    assert(g_gc_ctx != nullptr);
    g_gc_ctx->trigger_gc(async);
}

void* woomem_allocate_begin(size_t size)
{
    assert(g_gc_ctx != nullptr);
    g_gc_ctx->m_new_allocated_size_since_last_gc.fetch_add(size, std::memory_order_relaxed);

    if (size <= MAX_IN_PAGE_UNIT_SIZE)
    {
        return t_thread_context.m_thread_page_collection.pick_unit_in_page(size);
    }

    PageHead* const huge_unit_page =
        g_global_context.allocate_huge_page(sizeof(PageHead) + sizeof(UnitHead) + size);

    if (huge_unit_page == nullptr)
        return nullptr;

    UnitHead* const huge_unit_head =
        reinterpret_cast<UnitHead*>(huge_unit_page + 1);

    huge_unit_head->m_next_free_unit_offset = 0;
    huge_unit_head->m_life.store(
        UnitLife::PENDING,
        // relaxed is enough, `m_page_just_allocated` will be set as release order.
        std::memory_order::memory_order_relaxed);

    g_global_context.add_new_page_into_chain(huge_unit_page);

    return huge_unit_head + 1;
}
void woomem_allocate_end(void* p, int attrib)
{
    UnitHead* const unit_head =
        reinterpret_cast<UnitHead*>(p) - 1;

    unit_head->m_age = 15;
    unit_head->m_timing = woomem_gc_marking_round_counter;
    unit_head->m_attribute = static_cast<uint8_t>(attrib);
    unit_head->m_life.store(UnitLife::UNMARKED, std::memory_order::memory_order_release);
}
void woomem_allocate_end_as_root(void* p, int attrib)
{
    UnitHead* const unit_head =
        reinterpret_cast<UnitHead*>(p) - 1;

    woomem::g_gc_ctx->register_root_unit_head(unit_head);
    woomem_allocate_end(p, attrib);
}

void woomem_remove_from_root_set(void* p)
{
    UnitHead* const unit_head =
        reinterpret_cast<UnitHead*>(p) - 1;

    woomem::g_gc_ctx->unregister_root_unit_head(unit_head);
}

void* woomem_reallocate(void* ptr, size_t size)
{
    assert(ptr != nullptr);

    UnitHead* const unit_head =
        reinterpret_cast<UnitHead*>(ptr) - 1;

    const size_t existed_unit_available_space = unit_head->get_unit_available_size();

    if (size <= existed_unit_available_space)
        // TODO: Need a suitable shrink strategy to reallocate 
        //      memory under certain circumstances.
        return ptr;

    // Need reallocate.
    void* new_ptr = woomem_allocate_begin(size);
    if (new_ptr == nullptr)
        return nullptr;

    memcpy(new_ptr, ptr, existed_unit_available_space);

    woomem_allocate_end(new_ptr, unit_head->m_attribute);

    // TODO: A GC write barrier needs to be inserted after this point.
    // NOTE: The old cells will be freed by the GC.
    return new_ptr;
}

void* woomem_validate_addr(void* ptr_may_invalid)
{
    PageHead* const page_head = g_global_context.chunk().validate(ptr_may_invalid);
    if (page_head != nullptr
        && !page_head->m_page_just_allocated.load(std::memory_order::memory_order_acquire))
    {
        UnitHead* unit_head;
        if (page_head->m_page_count_if_huge == 0)
        {
            PageUnitAlloc* const page_alloc_head =
                reinterpret_cast<PageUnitAlloc*>(page_head + 1);

            const size_t unit_size_with_head =
                page_alloc_head->m_unit_size_in_page + sizeof(UnitHead);

            const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr_may_invalid);
            const uintptr_t storage_begin = reinterpret_cast<uintptr_t>(page_alloc_head + 1);

            if (addr < storage_begin)
                return nullptr;

            const size_t offset_in_units = addr - storage_begin;
            const size_t unit_index = offset_in_units / unit_size_with_head;

            unit_head = reinterpret_cast<UnitHead*>(
                storage_begin + unit_index * unit_size_with_head);
        }
        else
        {
            unit_head = reinterpret_cast<UnitHead*>(page_head + 1);
        }

        if (unit_head->m_life.load(std::memory_order::memory_order_relaxed) > UnitLife::PENDING)
            return unit_head + 1;
    }

    return nullptr;
}
void* woomem_validate_addr_head(void* ptr_may_invalid)
{
    if ((reinterpret_cast<intptr_t>(ptr_may_invalid) & 0b0111) == 0)
    {
        return woomem_validate_addr(ptr_may_invalid);
    }
    return nullptr;
}

void woomem_mark_unit_head(void* ptr_head_may_null)
{
    if (ptr_head_may_null != nullptr)
    {
        t_thread_context.m_gc_marking_context->mark_unit_to_gray(
            reinterpret_cast<UnitHead*>(ptr_head_may_null) - 1);
    }
}
void woomem_mark_fuzzy_unit(void* ptr_may_invalid_or_null)
{
    woomem_mark_unit_head(woomem_validate_addr(ptr_may_invalid_or_null));
}
void woomem_mark_fuzzy_unit_head(void* ptr_head_may_invalid_null)
{
    woomem_mark_unit_head(woomem_validate_addr_head(ptr_head_may_invalid_null));
}

void woomem_mark_root_unit_head(void* ptr_head_may_null)
{
    if (ptr_head_may_null != nullptr)
    {
        g_gc_ctx->mark_root_unit_to_gray(
            reinterpret_cast<UnitHead*>(ptr_head_may_null) - 1);
    }
}
void woomem_mark_root_fuzzy_unit(void* ptr_may_invalid_or_null)
{
    woomem_mark_root_unit_head(woomem_validate_addr(ptr_may_invalid_or_null));
}
void woomem_mark_root_fuzzy_unit_head(void* ptr_head_may_invalid_null)
{
    woomem_mark_root_unit_head(woomem_validate_addr_head(ptr_head_may_invalid_null));
}
