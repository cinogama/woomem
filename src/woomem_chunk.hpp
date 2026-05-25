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
        static constexpr uint32_t INDEX_NULL = UINT32_MAX;

        static constexpr uint8_t STATE_FREE        = 0x00;
        static constexpr uint8_t STATE_ALLOCATED   = 0x01;
        static constexpr uint8_t STATE_ORDER_SHIFT = 1;
        static constexpr uint8_t STATE_CONTINUATION = 0xFF;

        static size_t round_up_power_of_2(size_t v);
        static size_t ilog2(size_t v);

        size_t page_to_index(PageHead* page) const;
        PageHead* index_to_page(size_t idx) const;
        size_t addr_to_index(void* ptr) const;

        PageHead* allocate_block(size_t order);
        PageHead* commit_page(size_t idx);

        void push_free(size_t order, uint32_t idx);
        uint32_t pop_free(size_t order);
        void remove_free(size_t order, uint32_t idx);

        void*       base_;
        size_t      reserved_size_;
        size_t      total_pages_;
        size_t      max_order_;

        uint8_t*    state_;
        uint32_t*   prev_;
        uint32_t*   next_;
        uint32_t*   free_heads_;
        uint8_t*    commit_;

        ReadWriteSpinlock rwlock_;
    };
}
