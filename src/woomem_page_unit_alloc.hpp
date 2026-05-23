#pragma once

#include <cstdint>
#include <cstddef>
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
    static constexpr size_t GROUP_SIZE_LOOKUP_TABLE[MAX_GROUP] = {
        16, 40, 88, 168, 344, 520, 736, 984, 1480, 2948, 5448, 8176, 16360
    };
    static_assert(GROUP_SIZE_LOOKUP_TABLE[MIDIUM_16360] == 16360);

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
        uint32_t                __reserved__;
    };
    static_assert(sizeof(PageUnitAlloc) == 8);

    struct UnitHead
    {
        uint16_t            m_next_free_unit_offset;
        uint16_t            __reserved__;
        uint8_t             m_age;
        uint8_t             m_timing;
        std::atomic_uint8_t m_life;
        uint8_t             m_attribute;
    };
    static_assert(sizeof(UnitHead) == 8);

    void init_page_for_unit_allocating(PageHead* page, UnitAllocGroup group_type);
}
