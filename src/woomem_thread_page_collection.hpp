#pragma once

#include <atomic>
#include <cassert>

#include "woomem_chunk.hpp"
#include "woomem_page.hpp"
#include "woomem_page_unit_alloc.hpp"
#include "woomem_global_page_collection.hpp"

namespace woomem
{
    class ThreadPageCollection
    {
        GlobalPageCollection* m_global_page_collection;

    public:
        ThreadPageCollection(GlobalPageCollection* global_page_collection)
            : m_global_page_collection(global_page_collection)
        {}
        ~ThreadPageCollection()
        {}

        ThreadPageCollection(const ThreadPageCollection&) = delete;
        ThreadPageCollection& operator=(const ThreadPageCollection&) = delete;
        ThreadPageCollection(ThreadPageCollection&&) = delete;
        ThreadPageCollection& operator=(ThreadPageCollection&&) = delete;

    public:
        void* allocate_unit_in_page(size_t unit_size)
        {
            assert(unit_size <= MAX_IN_PAGE_UNIT_SIZE);

            UnitAllocGroup belong_group;

            if (unit_size <= MAX_SMALL_UNIT_SIZE)
                belong_group = SMALL_UNIT_GROUP_FAST_LOOKUP_TABLE[unit_size >> SMALL_UNIT_FAST_LOOKUP_SHIFT];
            else
            {
                if (unit_size < GROUP_SIZE_LOOKUP_TABLE[UnitAllocGroup::MIDIUM_1480])
                    belong_group = UnitAllocGroup::MIDIUM_1480;
                else if (unit_size < GROUP_SIZE_LOOKUP_TABLE[UnitAllocGroup::MIDIUM_2948])
                    belong_group = UnitAllocGroup::MIDIUM_2948;
                else if (unit_size < GROUP_SIZE_LOOKUP_TABLE[UnitAllocGroup::MIDIUM_5448])
                    belong_group = UnitAllocGroup::MIDIUM_5448;
                else if (unit_size < GROUP_SIZE_LOOKUP_TABLE[UnitAllocGroup::MIDIUM_8176])
                    belong_group = UnitAllocGroup::MIDIUM_8176;
                else
                    belong_group = UnitAllocGroup::MIDIUM_16360;
            }


        }
    };
}