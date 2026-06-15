#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>

namespace woomem
{
    struct PageHead
    {
        static constexpr size_t NORMAL_PAGE_SIZE = 32768;
        // =================================================
        alignas(8) size_t       m_page_count_if_huge;
        alignas(8) PageHead*    m_next_page;
        alignas(8) std::atomic_bool        m_page_just_allocated;
    };
    static_assert(sizeof(PageHead) == 24);
}