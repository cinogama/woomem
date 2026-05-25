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
        ThreadPageCollection(/* OPTIONAL */ GlobalPageCollection* global_page_collection)
            : m_global_page_collection(global_page_collection)
            , m_cached_pages{}
        {
            /*
            NOTE: global_page_collection might be nullptr if woomem is not inited yet.
            */
        }
        ~ThreadPageCollection()
        {
            shutdown_manually();
        }

        ThreadPageCollection(const ThreadPageCollection&) = delete;
        ThreadPageCollection& operator=(const ThreadPageCollection&) = delete;
        ThreadPageCollection(ThreadPageCollection&&) = delete;
        ThreadPageCollection& operator=(ThreadPageCollection&&) = delete;

        void init_manually(GlobalPageCollection* global_page_collection)
        {
            assert(m_global_page_collection == nullptr);
            m_global_page_collection = global_page_collection;
        }
        void shutdown_manually()
        {
            if (m_global_page_collection != nullptr)
            {
                for (int i = 0; i < UnitAllocGroup::MAX_GROUP; ++i)
                {
                    PageHead* page = m_cached_pages[i];
                    if (page != nullptr)
                        m_global_page_collection->return_page(page, static_cast<UnitAllocGroup>(i));
                }
                m_global_page_collection = nullptr;
            }
        }

    public:
        void* pick_unit_in_page(size_t unit_size)
        {
            assert(m_global_page_collection != nullptr && unit_size <= MAX_IN_PAGE_UNIT_SIZE);

            const UnitAllocGroup belong_group = eval_group_by_small_unit_size(unit_size);

            PageHead*& cached_page = m_cached_pages[belong_group];
            if (cached_page != nullptr)
            {
            _label_retry_allocate_with_new_page:
                UnitHead* const unit = pick_unit_from_page_without_init(cached_page);
                if (unit != nullptr)
                    return unit + 1;
            }

            cached_page = m_global_page_collection->require_normal_page(belong_group);
            if (cached_page == nullptr)
                return nullptr;

            goto _label_retry_allocate_with_new_page;
        }
    };
}