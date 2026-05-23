#pragma once

#include "woomem.h"

#include "woomem_page.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>

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

        PageHead* allocate_page();
        PageHead* allocate_huge_page(size_t size);
        void free_page(PageHead* page);

        PageHead* validate(void* ptr);

        void defragment();

        size_t get_total_size() const;
        size_t get_total_page_count() const;

    private:
        static constexpr uint32_t INDEX_NULL    = UINT32_MAX;
        static constexpr uint64_t PACKED_NULL   = UINT64_MAX;
        static constexpr uint8_t  STATE_ALLOCATED     = 0x01;
        static constexpr uint8_t  STATE_ORDER_SHIFT   = 1;
        static constexpr uint8_t  STATE_CONTINUATION  = 0xFF;

        static size_t round_up_power_of_2(size_t v);
        static size_t ilog2(size_t v);

        static uint64_t pack(uint32_t index, uint32_t counter);
        static uint32_t unpack_index(uint64_t packed);
        static uint32_t unpack_counter(uint64_t packed);

        size_t page_to_index(PageHead* page) const;
        PageHead* index_to_page(size_t idx) const;
        size_t addr_to_index(void* ptr) const;

        PageHead* allocate_block(size_t order);
        void remove_from_free_list_defrag(size_t order, uint32_t target);

        void* base_;
        size_t reserved_size_;
        size_t total_pages_;
        size_t max_order_;

        std::atomic<uint8_t>*  state_;
        std::atomic<uint64_t>* links_;
        std::atomic<uint64_t>* free_lists_;
        std::mutex              mutex_;
    };
}
