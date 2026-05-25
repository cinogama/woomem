#pragma once

#include <atomic>
#include <cassert>
#include <vector>

#include "woomem_chunk.hpp"
#include "woomem_page.hpp"
#include "woomem_page_unit_alloc.hpp"

namespace woomem
{
    class GlobalPageCollection
    {
        struct FreePageList
        {
            std::atomic_flag        m_spin;
            std::vector<PageHead*>  m_pages;

            FreePageList()
            {
                m_spin.clear();
            }

            FreePageList(const FreePageList&) = delete;
            FreePageList(FreePageList&&) = delete;
            FreePageList& operator=(const FreePageList&) = delete;
            FreePageList& operator=(FreePageList&&) = delete;

            PageHead* pick_free_page()
            {
                while (m_spin.test_and_set(std::memory_order_acquire))
                    ;

                PageHead* page = nullptr;
                if (!m_pages.empty())
                {
                    page = m_pages.back();
                    m_pages.pop_back();
                }

                m_spin.clear(std::memory_order_release);
                return page;
            }
            void return_free_page(PageHead* page)
            {
                while (m_spin.test_and_set(std::memory_order_acquire))
                    ;

                m_pages.push_back(page);

                m_spin.clear(std::memory_order_release);
            }
        };

        Chunk* m_chunk;
        FreePageList m_free_pages[UnitAllocGroup::MAX_GROUP];
    public:
        GlobalPageCollection(Chunk* chunk)
            : m_chunk(chunk)
        {
            assert(chunk != nullptr && !chunk->is_init_failed());
        }

        GlobalPageCollection(const GlobalPageCollection&) = delete;
        GlobalPageCollection(GlobalPageCollection&&) = delete;
        GlobalPageCollection& operator=(const GlobalPageCollection&) = delete;
        GlobalPageCollection& operator=(GlobalPageCollection&&) = delete;

    public:
        PageHead* require_normal_page(UnitAllocGroup group)
        {
            PageHead* page = m_free_pages[group].pick_free_page();
            if (page != nullptr)
            {
                assert(page->m_page_count_if_huge == 0
                    && reinterpret_cast<PageUnitAlloc*>(page + 1)->m_run_out == false
                    && reinterpret_cast<PageUnitAlloc*>(page + 1)->m_unit_size_in_page == GROUP_SIZE_LOOKUP_TABLE[group]);

                return page;
            }

            page = m_chunk->allocate_page();
            if (page != nullptr)
            {
                init_page_for_unit_allocating(page, group);
            }
            return page;
        }
        void return_page(PageHead* page, UnitAllocGroup group)
        {
            m_free_pages[group].return_free_page(page);
        }
    };
}