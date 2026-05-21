#include "woomem.h"

#include "woomem_page.hpp"

#include <cstdint>

namespace woomem
{
    class Chunk
    {
    public:
        Chunk(size_t reserved_size);
        ~Chunk();

        Chunk(const Chunk&) = delete;
        Chunk(Chunk&&) = delete;
        Chunk& operator = (const Chunk&) = delete;
        Chunk& operator = (Chunk&&) = delete;

        Page* allocate_page();
        Page* allocate_huge_page(size_t size);
        void free_page(Page* page);

        Page* validate(void* ptr);

    private:
        static constexpr uint32_t FREE_LIST_NULL = UINT32_MAX;
        static constexpr uint8_t STATE_ALLOCATED = 0x01;
        static constexpr uint8_t STATE_ORDER_SHIFT = 1;

        static size_t round_up_power_of_2(size_t v);
        static size_t ilog2(size_t v);

        size_t page_to_index(Page* page) const;
        Page* index_to_page(size_t idx) const;
        size_t addr_to_index(void* ptr) const;

        void remove_from_free_list(size_t order, uint32_t target);

        Page* allocate_block(size_t order);

        void* base_;
        size_t reserved_size_;
        size_t total_pages_;
        size_t max_order_;

        uint8_t* state_;
        uint32_t* links_;
        uint32_t* free_lists_;
    };
}