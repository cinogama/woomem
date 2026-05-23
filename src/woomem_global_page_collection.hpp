#pragma once

#include <atomic>
#include <cassert>

#include "woomem_chunk.hpp"
#include "woomem_page.hpp"
#include "woomem_page_unit_alloc.hpp"

namespace woomem
{
    class GlobalPageCollection
    {
        Chunk* m_chunk;
        std::atomic<PageHead*> m_free_pages[UnitAllocGroup::MAX_GROUP];
    public:
        GlobalPageCollection(Chunk* chunk)
            : m_chunk(chunk)
            , m_free_pages{}
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
            PageHead* page = m_free_pages[group].load(std::memory_order_relaxed);
            while (page != nullptr)
            {
                PageHead* next = page->m_next_page;
                if (m_free_pages[group].compare_exchange_weak(
                    page, next,
                    std::memory_order_release,
                    std::memory_order_relaxed))
                {
                    return page;
                }
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
            PageHead* head = m_free_pages[group].load(std::memory_order_relaxed);
            do
            {
                page->m_next_page = head;
            } while (!m_free_pages[group].compare_exchange_weak(
                head, page,
                std::memory_order_release,
                std::memory_order_relaxed));
        }
    };
}