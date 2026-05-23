#pragma once

namespace woomem
{
    struct PageHead
    {
        static constexpr size_t NORMAL_PAGE_SIZE = 32768;
        // =================================================

        PageHead*   m_next_page;
        size_t      m_size_if_huge_page;
    };
}