#include "woomem_page.hpp"
#include "woomem_page_unit_alloc.hpp"

#include "woomem_global_context.hpp"

#include <cassert>

namespace woomem
{
    void init_page_for_unit_allocating(PageHead* page, UnitAllocGroup group_type)
    {
        PageUnitAlloc* const page_alloc_head =
            reinterpret_cast<PageUnitAlloc*>(page + 1);

        page_alloc_head->m_run_out.store(0, std::memory_order::memory_order_relaxed);
        page_alloc_head->m_freed_unit_offset.store(0, std::memory_order::memory_order_relaxed);
        page_alloc_head->m_next_allocate_unit_offset = sizeof(PageUnitAlloc);
        page_alloc_head->m_unit_size_in_page =
            static_cast<uint16_t>(GROUP_SIZE_LOOKUP_TABLE[group_type]);

        const size_t group_unit_size_include_unit_head =
            sizeof(UnitHead) + page_alloc_head->m_unit_size_in_page;

        const size_t available_for_units =
            PageHead::NORMAL_PAGE_SIZE - (sizeof(PageHead) + sizeof(PageUnitAlloc));
        const size_t unit_count =
            available_for_units / group_unit_size_include_unit_head;

        size_t offset = sizeof(PageUnitAlloc);
        for (size_t i = 0; i < unit_count; i++)
        {
            UnitHead* const current_unit =
                reinterpret_cast<UnitHead*>(
                    reinterpret_cast<char*>(page_alloc_head) + offset);

            offset += group_unit_size_include_unit_head;

            current_unit->m_next_free_unit_offset =
                (i + 1 < unit_count) ? static_cast<uint16_t>(offset) : 0;
            current_unit->m_age = 0;
            current_unit->m_timing = 0;
            current_unit->m_attribute = 0;
            current_unit->m_life.store(
                UnitLife::RELEASED, std::memory_order::memory_order_relaxed);

        }

        // NOTE: No need for fence. new allocated page will be used for current thread.
        //      If drop back to global list, there will be a release/acquire order.
        g_global_context.add_page_into_chain(page);
    }
}