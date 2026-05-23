#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>

namespace woomem
{
    enum UnitAllocGroup
    {
        SMALL_16,
        SMALL_40,
        SMALL_88,
        SMALL_168,
        SMALL_240,
        SMALL_344,
        SMALL_704,
        SMALL_920,
        SMALL_1024,

        MIDIUM_2048,
        MIDIUM_4096,
        MIDIUM_8192,
        MIDIUM_16384,
    };

    /*
    * (32768 - (16 + 8)) % (x + 8)
    [PageHead] [PageUnitAlloc] [[UnitHead][       ] [UnitHead][       ] [UnitHead][       ]...]
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
}
