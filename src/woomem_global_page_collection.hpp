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
        }
    };
}