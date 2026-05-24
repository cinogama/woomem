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

        union
        {
            struct
            {
                std::atomic_bool
                            m_page_just_allocated;

                size_t      m_page_count_if_huge;
                PageHead*   m_next_page;
            };
            char __reserved__[24];
        };
    };
    static_assert(sizeof(PageHead) == 24);
}