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
        SMALL_984,
        MAX_SMALL_GROUP = SMALL_984,

        MIDIUM_1480,
        MIDIUM_2948,
        MIDIUM_5448,
        MIDIUM_8176,
        MIDIUM_16360,

        MAX_GROUP
    };

    static constexpr size_t MAX_SMALL_UNIT_SIZE = 984;
    static constexpr size_t MAX_IN_PAGE_UNIT_SIZE = 16360;

    static constexpr size_t GROUP_SIZE_LOOKUP_TABLE[MAX_GROUP] = {
        16, 40, 88, 168, 344, 520, 736, 984, 1480, 2948, 5448, 8176, 16360
    };
    static_assert(GROUP_SIZE_LOOKUP_TABLE[MIDIUM_16360] == MAX_IN_PAGE_UNIT_SIZE);

    static constexpr size_t SMALL_UNIT_FAST_LOOKUP_SHIFT = 3;
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
        // 737~984(123)
        SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984,
        SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984,
        SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984,
        SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984, SMALL_984,
    };
    static_assert(
        sizeof(SMALL_UNIT_GROUP_FAST_LOOKUP_TABLE) / sizeof(UnitAllocGroup)
        == (MAX_SMALL_UNIT_SIZE >> SMALL_UNIT_FAST_LOOKUP_SHIFT) + 1);

    /*
    * (32768 - (16 + 8)) % (x + 8)
    * Page layout: [PageHead(16)] [PageUnitAlloc(8)] [[UnitHead(8)][payload(x)]]...
    * Available per page: 32744 bytes.
    */

    struct PageUnitAlloc
    {
        uint16_t                m_next_allocate_unit_offset;
        std::atomic_uint16_t    m_freed_unit_offset;
        std::atomic_uint8_t     m_run_out;
        char                    __reserved__[3];
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
        uint16_t            m_next_free_unit_offset;
        char                __reserved__[2];
        uint8_t             m_age;
        uint8_t             m_timing;
        std::atomic_uint8_t m_life;
        uint8_t             m_attribute;
    };
    static_assert(sizeof(UnitHead) == 8);

    void init_page_for_unit_allocating(PageHead* page, UnitAllocGroup group_type);
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
                    reinterpret_cast<char*>(page) + current_offset);

                page_alloc_head->m_next_allocate_unit_offset =
                    allocating_unit->m_next_free_unit_offset;

                assert(UnitLife::RELEASED == allocating_unit->m_life.load(
                    std::memory_order::memory_order_relaxed));

                return allocating_unit;
            }

            current_offset = page_alloc_head->m_freed_unit_offset.exchange(
                0, 
                std::memory_order::memory_order_acquire);

            if (current_offset == 0)
            {
                page_alloc_head->m_run_out.store(
                    1, 
                    std::memory_order::memory_order_relaxed);

                return nullptr;
            }

            page_alloc_head->m_next_allocate_unit_offset = current_offset;

        } while (1);
    }
    inline void drop_freed_unit_into_page(PageHead* page, UnitHead* unit)
    {
        const auto unit_offset = static_cast<uint16_t>(
            reinterpret_cast<char*>(unit) - reinterpret_cast<char*>(page));
        PageUnitAlloc* const page_alloc_head = reinterpret_cast<PageUnitAlloc*>(page + 1);

        assert(UnitLife::RELEASED == unit->m_life.load(
            std::memory_order::memory_order_relaxed));

        unit->m_next_free_unit_offset = page_alloc_head->m_freed_unit_offset.load(
            std::memory_order::memory_order_relaxed);
        do
        {
        } while (!page_alloc_head->m_freed_unit_offset.compare_exchange_weak(
            unit->m_next_free_unit_offset,
            unit_offset,
            std::memory_order::memory_order_release,
            std::memory_order::memory_order_relaxed));
    }
}