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
        PageHead* m_cached_pages[UnitAllocGroup::MAX_GROUP];

    public:
        ThreadPageCollection(GlobalPageCollection* global_page_collection)
            : m_global_page_collection(global_page_collection)
            , m_cached_pages{}
        {}
        ~ThreadPageCollection()
        {
            for (int i = 0; i < UnitAllocGroup::MAX_GROUP; ++i)
            {
                PageHead* page = m_cached_pages[i];
                if (page != nullptr)
                    m_global_page_collection->return_page(page, static_cast<UnitAllocGroup>(i));
            }
        }

        ThreadPageCollection(const ThreadPageCollection&) = delete;
        ThreadPageCollection& operator=(const ThreadPageCollection&) = delete;
        ThreadPageCollection(ThreadPageCollection&&) = delete;
        ThreadPageCollection& operator=(ThreadPageCollection&&) = delete;

    public:
        UnitHead* pick_unit_in_page(size_t unit_size)
        {
            assert(unit_size <= MAX_IN_PAGE_UNIT_SIZE);

            UnitAllocGroup belong_group;

            if (unit_size <= MAX_SMALL_UNIT_SIZE)
                belong_group = SMALL_UNIT_GROUP_FAST_LOOKUP_TABLE[
                    WOOMEM_FAST_LOOKUP_GROUP_INDEX(unit_size)];
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

            PageHead* cached_page = m_cached_pages[belong_group];
            if (cached_page != nullptr)
            {
            _label_retry_allocate_with_new_page:
                UnitHead* const unit = pick_unit_from_page_without_init(cached_page);
                if (unit != nullptr)
                    return unit;
            }

            cached_page = m_global_page_collection->require_normal_page(belong_group);
            if (cached_page == nullptr)
                return nullptr;

            m_cached_pages[belong_group] = cached_page;
            goto _label_retry_allocate_with_new_page;
        }
    };
}