#include "woomem_page.hpp"
#include "woomem_page_unit_alloc.hpp"

namespace woomem
{
    void init_page_for_unit_allocating(PageHead* page, UnitAllocGroup group_type)
    {
        PageUnitAlloc* const page_alloc_head =
            reinterpret_cast<PageUnitAlloc*>(page + 1);
        char* const unit_real_storage_place =
            reinterpret_cast<char*>(page_alloc_head + 1);

        constexpr size_t UNIT_PAGE_HEAD_SIZE = sizeof(PageHead) + sizeof(PageUnitAlloc);

        page_alloc_head->m_freed_unit_offset.store(0, std::memory_order::memory_order_relaxed);
        page_alloc_head->m_next_allocate_unit_offset = UNIT_PAGE_HEAD_SIZE;

        const size_t group_unit_size_include_unit_head =
            sizeof(UnitHead) + GROUP_SIZE_LOOKUP_TABLE[group_type];

        for (size_t offset = 0; offset < PageHead::NORMAL_PAGE_SIZE - UNIT_PAGE_HEAD_SIZE; offset)
        {
        }

    }
}