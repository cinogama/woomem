#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <atomic>
#include <array>
#include <utility>

#include "woomem_page.hpp"

namespace woomem
{
    enum UnitAllocGroup
    {
        SMALL_16,
        SMALL_40,
        SMALL_88,
        SMALL_168,
        SMALL_344,
        SMALL_520,
        SMALL_736,
        SMALL_1048,
        MAX_SMALL_GROUP = SMALL_1048,

        MIDIUM_2720,
        MIDIUM_5448,
        MIDIUM_8176,
        MIDIUM_10904,
        MIDIUM_16360,

        MAX_GROUP
    };

    static constexpr size_t MAX_SMALL_UNIT_SIZE = 1048;
    static constexpr size_t MAX_IN_PAGE_UNIT_SIZE = 16360;

    static constexpr size_t GROUP_SIZE_LOOKUP_TABLE[MAX_GROUP] = {
        16, 40, 88, 168, 344, 520, 736, 1048, 2720, 5448, 8176, 10904, 16360
    };
    static_assert(GROUP_SIZE_LOOKUP_TABLE[MIDIUM_16360] == MAX_IN_PAGE_UNIT_SIZE);

    static constexpr UnitAllocGroup SMALL_UNIT_GROUP_FAST_LOOKUP_TABLE[] =
    {
        // 0
        SMALL_16,
        // 1~16(2)
        SMALL_16, SMALL_16,
        // 17~40(5)
        SMALL_40, SMALL_40, SMALL_40,
        // 41~88(11)
        SMALL_88, SMALL_88, SMALL_88, SMALL_88, SMALL_88, SMALL_88,
        // 89~168(21)
        SMALL_168, SMALL_168, SMALL_168, SMALL_168, SMALL_168, SMALL_168, SMALL_168, SMALL_168,
        SMALL_168, SMALL_168,
        // 169~344(43)
        SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344,
        SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344,
        SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344, SMALL_344,
        // 345~520(65)
        SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520,
        SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520,
        SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520, SMALL_520,
        // 521~736(92)
        SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736,
        SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736,
        SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736, SMALL_736,
        SMALL_736, SMALL_736, SMALL_736,
        // 737~1048(131)
        SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048,
        SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048,
        SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048,
        SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048,
        SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048, SMALL_1048,
    };
#define WOOMEM_FAST_LOOKUP_GROUP_INDEX(SIZE) (((SIZE) + 7) >> 3)
    static_assert(
        sizeof(SMALL_UNIT_GROUP_FAST_LOOKUP_TABLE) / sizeof(UnitAllocGroup)
        == WOOMEM_FAST_LOOKUP_GROUP_INDEX(MAX_SMALL_UNIT_SIZE) + 1);

    /*
    * (32768 - (24 + 8)) % (x + 8)
    * Page layout: [PageHead(24)] [PageUnitAlloc(8)] [[UnitHead(8)][payload(x)]]...
    * Available per page: 32736 bytes.
    */

    struct PageUnitAlloc
    {
        uint16_t                m_next_allocate_unit_offset;
        std::atomic_uint16_t    m_freed_unit_offset;
        bool                    m_run_out;
        char                    __reserved__[1];
        uint16_t                m_unit_size_in_page;
    };
    static_assert(sizeof(PageUnitAlloc) == 8);

    enum UnitLife : uint8_t
    {
        RELEASED,
        UNMARKED,
        SELF_MARKED,
        FULL_MARKED,
    };

    struct UnitHead
    {
        uint16_t            m_next_free_unit_offset /* m_unit_offset_in_page */;
        char                __reserved__[2];
        uint8_t             m_age;
        uint8_t             m_timing;
        uint8_t             m_attribute;
        std::atomic_uint8_t m_life;

        size_t get_unit_available_size() const
        {
            if (m_next_free_unit_offset != 0)
            {
                const PageUnitAlloc* const unit_alloc_page =
                    reinterpret_cast<const PageUnitAlloc*>(
                        reinterpret_cast<const char*>(this)
                        - m_next_free_unit_offset);

                return unit_alloc_page->m_unit_size_in_page;
            }
            else
            {
                const PageHead* const huge_page =
                    reinterpret_cast<const PageHead*>(this) - 1;

                assert(huge_page->m_page_count_if_huge != 0);
                return huge_page->m_page_count_if_huge * PageHead::NORMAL_PAGE_SIZE
                    - (sizeof(PageHead) + sizeof(UnitHead));
            }
        }
    };
    static_assert(sizeof(UnitHead) == 8);

    void init_page_for_unit_allocating(PageHead* page, UnitAllocGroup group_type);
    inline UnitAllocGroup eval_group_by_small_unit_size(size_t unit_size)
    {
        if (unit_size <= MAX_SMALL_UNIT_SIZE)
            return SMALL_UNIT_GROUP_FAST_LOOKUP_TABLE[
                WOOMEM_FAST_LOOKUP_GROUP_INDEX(unit_size)];
        else
        {
            if (unit_size <= GROUP_SIZE_LOOKUP_TABLE[UnitAllocGroup::MIDIUM_2720])
                return UnitAllocGroup::MIDIUM_2720;
            else if (unit_size <= GROUP_SIZE_LOOKUP_TABLE[UnitAllocGroup::MIDIUM_5448])
                return UnitAllocGroup::MIDIUM_5448;
            else if (unit_size <= GROUP_SIZE_LOOKUP_TABLE[UnitAllocGroup::MIDIUM_8176])
                return UnitAllocGroup::MIDIUM_8176;
            else if (unit_size <= GROUP_SIZE_LOOKUP_TABLE[UnitAllocGroup::MIDIUM_10904])
                return UnitAllocGroup::MIDIUM_10904;
            else
                return UnitAllocGroup::MIDIUM_16360;
        }
    }
    inline UnitHead* pick_unit_from_page_without_init(PageHead* page)
    {
        constexpr uint16_t UNIT_PAGE_HEAD_SIZE =
            static_cast<uint16_t>(sizeof(PageHead) + sizeof(PageUnitAlloc));

        PageUnitAlloc* const page_alloc_head =
            reinterpret_cast<PageUnitAlloc*>(page + 1);

        uint16_t current_offset = page_alloc_head->m_next_allocate_unit_offset;
        do
        {
            if (current_offset != 0)
            {
                UnitHead* const allocating_unit = reinterpret_cast<UnitHead*>(
                    reinterpret_cast<char*>(page_alloc_head) + current_offset);

                page_alloc_head->m_next_allocate_unit_offset =
                    allocating_unit->m_next_free_unit_offset;
                allocating_unit->m_next_free_unit_offset =
                    current_offset;

                assert(UnitLife::RELEASED == allocating_unit->m_life.load(
                    std::memory_order::memory_order_relaxed));

                return allocating_unit;
            }

            current_offset = page_alloc_head->m_freed_unit_offset.exchange(
                0,
                std::memory_order::memory_order_acquire);

            if (current_offset == 0)
            {
                page_alloc_head->m_run_out = true;
                return nullptr;
            }

            page_alloc_head->m_next_allocate_unit_offset = current_offset;

        } while (1);
    }
    inline void drop_freed_unit_into_page(PageHead* page, UnitHead* unit)
    {
        PageUnitAlloc* const page_alloc_head =
            reinterpret_cast<PageUnitAlloc*>(page + 1);

        const auto unit_offset = static_cast<uint16_t>(
            reinterpret_cast<char*>(unit) - reinterpret_cast<char*>(page_alloc_head));

        assert(UnitLife::RELEASED == unit->m_life.load(
            std::memory_order::memory_order_relaxed));

        unit->m_next_free_unit_offset = page_alloc_head->m_freed_unit_offset.load(
            std::memory_order::memory_order_relaxed);

        while (!page_alloc_head->m_freed_unit_offset.compare_exchange_weak(
            unit->m_next_free_unit_offset,
            unit_offset,
            std::memory_order::memory_order_release,
            std::memory_order::memory_order_relaxed))
            /* Atomic retry */;
    }
}