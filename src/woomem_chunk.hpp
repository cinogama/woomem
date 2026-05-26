#pragma once

#include "woomem_page.hpp"
#include "woomem_rwlock.hpp"

#include <cstdint>
#include <cstddef>

namespace woomem
{
    class Chunk
    {
    public:
        Chunk(size_t reserved_size);
        ~Chunk();

        Chunk(const Chunk&) = delete;
        Chunk(Chunk&&) = delete;
        Chunk& operator=(const Chunk&) = delete;
        Chunk& operator=(Chunk&&) = delete;

        bool is_init_failed() const;

        PageHead* allocate_page();
        PageHead* allocate_huge_page(size_t size);
        void free_page(PageHead* page);

        PageHead* validate(void* ptr);

        size_t get_total_size() const;
        size_t get_total_page_count() const;

    private:
        static constexpr uint32_t INDEX_NULL       = UINT32_MAX;
        static constexpr uint32_t ALLOCATED_FLAG   = 0x80000000u;
        static constexpr uint32_t COUNT_MASK       = 0x7FFFFFFFu;

        size_t page_to_index(PageHead* page) const;
        PageHead* index_to_page(size_t idx) const;
        size_t addr_to_index(void* ptr) const;

        PageHead* commit_page(size_t idx);

        PageHead* allocate_pages(uint32_t required_pages);

        void free_list_insert(uint32_t idx, uint32_t count);
        void free_list_remove(uint32_t idx);

        uint32_t free_list_find_block(uint32_t required) const;

        void*       base_;
        size_t      reserved_size_;
        size_t      total_pages_;

        uint32_t*   count_;
        uint32_t*   free_prev_;
        uint32_t*   free_next_;
        uint32_t    free_head_;
        uint8_t*    commit_;

        ReadWriteSpinlock rwlock_;
    };
}
