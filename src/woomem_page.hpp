#pragma once

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
                PageHead*   m_next_page;
                size_t      m_size_if_huge_page;
            };
            char __reserved__[16];
        };
    };
    static_assert(sizeof(PageHead) == 16);
}