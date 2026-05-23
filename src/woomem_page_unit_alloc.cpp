#include "woomem_page.hpp"
#include "woomem_page_unit_alloc.hpp"

namespace woomem
{
    void init_page_for_unit_allocating(PageHead* page, UnitAllocGroup group_type)
    {
        PageUnitAlloc* const page_alloc_head =
            reinterpret_cast<PageUnitAlloc*>(page + 1);

        constexpr size_t UNIT_PAGE_HEAD_SIZE = sizeof(PageHead) + sizeof(PageUnitAlloc);

        page_alloc_head->m_run_out.store(0, std::memory_order::memory_order_relaxed);
        page_alloc_head->m_freed_unit_offset.store(0, std::memory_order::memory_order_relaxed);
        page_alloc_head->m_next_allocate_unit_offset = UNIT_PAGE_HEAD_SIZE;

        const size_t group_unit_size_include_unit_head =
            sizeof(UnitHead) + GROUP_SIZE_LOOKUP_TABLE[group_type];

        const size_t available_for_units =
            PageHead::NORMAL_PAGE_SIZE - UNIT_PAGE_HEAD_SIZE;
        const size_t unit_count =
            available_for_units / group_unit_size_include_unit_head;

        for (size_t i = 0; i < unit_count; i++)
        {
            const auto unit_offset =
                static_cast<uint16_t>(UNIT_PAGE_HEAD_SIZE + i * group_unit_size_include_unit_head);
            UnitHead* unit =
                reinterpret_cast<UnitHead*>(reinterpret_cast<char*>(page) + unit_offset);

            unit->m_next_free_unit_offset = (i + 1 < unit_count)
                ? static_cast<uint16_t>(unit_offset + group_unit_size_include_unit_head)
                : 0;
            unit->m_age = 0;
            unit->m_timing = 0;
            unit->m_life.store(UnitLife::RELEASED, std::memory_order::memory_order_relaxed);
            unit->m_attribute = 0;
        }
    }
}