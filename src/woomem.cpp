#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_global_context.hpp"
#include "woomem_thread_context.hpp"

#include "woomem_page_unit_alloc.hpp"

#include <cassert>
#include <cstring>

using namespace woomem;

bool woomem_is_gc_in_marking = false;

bool woomem_init(
    size_t reserved_chunk_size,
    woomem_MarkCallback mark_callback,
    woomem_FreeCallback free_callback,
    woomem_GCCallback gc_callback_at_begin)
{
    assert(!g_global_context.m_globalcontext_inited);
    return g_global_context.init(reserved_chunk_size);
}
void woomem_shutdown(void)
{
    g_global_context.shutdown();
}

// ======================================================================

void woomem_trigger_gc(bool async);

void* woomem_allocate_begin(size_t size)
{
    if (size <= MAX_IN_PAGE_UNIT_SIZE)
        return t_thread_context.m_thread_page_collection.pick_unit_in_page(size);

    PageHead* const huge_unit_page =
        g_global_context.allocate_huge_page(sizeof(PageHead) + sizeof(UnitHead) + size);

    if (huge_unit_page == nullptr)
        return nullptr;

    UnitHead* const huge_unit_head =
        reinterpret_cast<UnitHead*>(huge_unit_page + 1);

    huge_unit_head->m_next_free_unit_offset = 0;
    huge_unit_head->m_life.store(
        UnitLife::RELEASED,
        std::memory_order::memory_order_relaxed);

    g_global_context.add_page_into_chain(huge_unit_page);
    return huge_unit_head + 1;
}
void woomem_allocate_end(void* p, int attrib)
{
    UnitHead* const unit_head =
        reinterpret_cast<UnitHead*>(p) - 1;

    unit_head->m_attribute = static_cast<uint8_t>(attrib);
    unit_head->m_life.store(UnitLife::UNMARKED, std::memory_order::memory_order_release);
}
void* woomem_reallocate(void* ptr, size_t size)
{
    assert(ptr != nullptr);

    UnitHead* const unit_head =
        reinterpret_cast<UnitHead*>(ptr) - 1;

    size_t existed_unit_available_space;
    if (unit_head->m_next_free_unit_offset != 0)
    {
        PageUnitAlloc* const unit_alloc_page =
            reinterpret_cast<PageUnitAlloc*>(
                reinterpret_cast<char*>(unit_head)
                - unit_head->m_next_free_unit_offset);

        existed_unit_available_space =
            unit_alloc_page->m_unit_size_in_page;
    }
    else
    {
        // Is huge unit.
        PageHead* const huge_page =
            reinterpret_cast<PageHead*>(reinterpret_cast<UnitHead*>(ptr) - 1) - 1;

        assert(huge_page->m_page_count_if_huge != 0);
        existed_unit_available_space =
            huge_page->m_page_count_if_huge * PageHead::NORMAL_PAGE_SIZE
            - (sizeof(PageHead) + sizeof(UnitHead));
    }

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

void woomem_mark_unit_head(void* ptr_head_may_null);
void woomem_mark_fuzzy_unit(void* ptr_may_invalid_or_null);
