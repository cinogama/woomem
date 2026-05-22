#pragma once

#include "woomem.h"
#include "woomem_page.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace woomem
{
    class SpinLock
    {
        std::atomic<uint32_t> flag_{0};
    public:
        void lock() noexcept;
        void unlock() noexcept;
    };

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
        void defragment();

        Page* validate(void* ptr);

    private:
        static size_t round_up_power_of_two(size_t n);
        static int ceil_log2(size_t n);

        size_t alloc_block_(int order);
        void free_block_(size_t idx, int order);
        size_t buddy_of_(size_t idx, int order) const;

        static constexpr uint16_t FLAG_ALLOCATED = 0x8000;
        static constexpr uint16_t FLAG_IS_HEAD   = 0x4000;
        static constexpr uint16_t MASK_VALUE     = 0x3FFF;

        Page*     pages_        = nullptr;
        size_t    total_pages_  = 0;
        int       max_order_    = -1;

        uint16_t* page_state_   = nullptr;
        size_t*   free_heads_   = nullptr;
        size_t*   free_next_    = nullptr;

        SpinLock spinlock_;
    };
}
