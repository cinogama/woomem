#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_chunk.hpp"

#include <cstdlib>
#include <cstring>
#include <cassert>

namespace woomem
{
    size_t Chunk::round_up_power_of_2(size_t v)
    {
        if (v <= 1) return 1;
        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        if (sizeof(size_t) > 4) v |= v >> 32;
        return v + 1;
    }

    size_t Chunk::ilog2(size_t v)
    {
        size_t r = 0;
        while (v >>= 1) ++r;
        return r;
    }

    uint64_t Chunk::pack(uint32_t index, uint32_t counter)
    {
        return (static_cast<uint64_t>(counter) << 32) | index;
    }

    uint32_t Chunk::unpack_index(uint64_t packed)
    {
        return static_cast<uint32_t>(packed & 0xFFFFFFFFULL);
    }

    uint32_t Chunk::unpack_counter(uint64_t packed)
    {
        return static_cast<uint32_t>(packed >> 32);
    }

    size_t Chunk::page_to_index(PageHead* page) const
    {
        return static_cast<size_t>(
            reinterpret_cast<uintptr_t>(page) -
            reinterpret_cast<uintptr_t>(base_)) /
            PageHead::NORMAL_PAGE_SIZE;
    }

    PageHead* Chunk::index_to_page(size_t idx) const
    {
        return reinterpret_cast<PageHead*>(
            reinterpret_cast<uintptr_t>(base_) +
            idx * PageHead::NORMAL_PAGE_SIZE);
    }

    size_t Chunk::addr_to_index(void* ptr) const
    {
        return static_cast<size_t>(
            reinterpret_cast<uintptr_t>(ptr) -
            reinterpret_cast<uintptr_t>(base_)) /
            PageHead::NORMAL_PAGE_SIZE;
    }

    Chunk::Chunk(size_t reserved_size)
    {
        size_t num_pages =
            (reserved_size + PageHead::NORMAL_PAGE_SIZE - 1) /
            PageHead::NORMAL_PAGE_SIZE;
        total_pages_ = round_up_power_of_2(num_pages);
        max_order_ = ilog2(total_pages_);
        reserved_size_ = total_pages_ * PageHead::NORMAL_PAGE_SIZE;

        base_ = woomem_os_reserve_memory(reserved_size_);
        if (!base_)
        {
            total_pages_ = 0;
            max_order_ = 0;
            reserved_size_ = 0;
            state_ = nullptr;
            links_ = nullptr;
            free_lists_ = nullptr;
            return;
        }
        woomem_os_commit_memory(base_, reserved_size_);

        state_ = new std::atomic<uint8_t>[total_pages_];
        links_ = new std::atomic<uint64_t>[total_pages_];
        free_lists_ = new std::atomic<uint64_t>[max_order_ + 1];

        for (size_t i = 0; i < total_pages_; ++i)
        {
            state_[i].store(0, std::memory_order_relaxed);
            links_[i].store(PACKED_NULL, std::memory_order_relaxed);
        }

        for (size_t i = 0; i <= max_order_; ++i)
        {
            free_lists_[i].store(PACKED_NULL, std::memory_order_relaxed);
        }

        state_[0].store(
            static_cast<uint8_t>(max_order_ << STATE_ORDER_SHIFT),
            std::memory_order_release);

        uint64_t old_head =
            free_lists_[max_order_].load(std::memory_order_acquire);
        uint64_t new_head;
        do
        {
            links_[0].store(old_head, std::memory_order_release);
            new_head = pack(0, unpack_counter(old_head) + 1);
        } while (!free_lists_[max_order_].compare_exchange_weak(
            old_head, new_head,
            std::memory_order_release,
            std::memory_order_acquire));
    }

    Chunk::~Chunk()
    {
        delete[] state_;
        delete[] links_;
        delete[] free_lists_;
        if (base_)
        {
            woomem_os_release_memory(base_, reserved_size_);
        }
    }

    PageHead* Chunk::allocate_block(size_t order)
    {
        assert(base_ != nullptr);

        if (order > max_order_)
            return nullptr;

        for (size_t k = order; k <= max_order_; ++k)
        {
            uint64_t old_head =
                free_lists_[k].load(std::memory_order_acquire);
            while (old_head != PACKED_NULL)
            {
                uint32_t idx = unpack_index(old_head);
                uint64_t next = links_[idx].load(std::memory_order_acquire);
                uint64_t new_head;

                if (next == PACKED_NULL)
                {
                    new_head = PACKED_NULL;
                }
                else
                {
                    new_head = pack(
                        unpack_index(next),
                        unpack_counter(old_head) + 1);
                }

                if (free_lists_[k].compare_exchange_weak(
                    old_head, new_head,
                    std::memory_order_release,
                    std::memory_order_acquire))
                {
                    while (k > order)
                    {
                        --k;
                        size_t buddy = idx + (size_t(1) << k);

                        state_[buddy].store(
                            static_cast<uint8_t>(k << STATE_ORDER_SHIFT),
                            std::memory_order_release);

                        uint64_t old_fl =
                            free_lists_[k].load(std::memory_order_acquire);
                        uint64_t new_fl;
                        do
                        {
                            links_[buddy].store(
                                old_fl, std::memory_order_release);
                            new_fl = pack(
                                static_cast<uint32_t>(buddy),
                                unpack_counter(old_fl) + 1);
                        } while (!free_lists_[k].compare_exchange_weak(
                            old_fl, new_fl,
                            std::memory_order_release,
                            std::memory_order_acquire));
                    }

                    state_[idx].store(
                        static_cast<uint8_t>(
                            (order << STATE_ORDER_SHIFT) | STATE_ALLOCATED),
                        std::memory_order_release);

                    size_t block_pages = size_t(1) << order;
                    for (size_t j = 1; j < block_pages; ++j)
                    {
                        state_[idx + j].store(
                            STATE_CONTINUATION, std::memory_order_release);
                    }

                    return index_to_page(idx);
                }
            }
        }
        return nullptr;
    }

    PageHead* Chunk::allocate_page()
    {
        PageHead* page = allocate_block(0);
        if (page != nullptr)
            page->m_size_if_huge_page = 0;
        return page;
    }

    PageHead* Chunk::allocate_huge_page(size_t size)
    {
        assert(base_ != nullptr && size != 0);

        const size_t required_pages =
            (size + PageHead::NORMAL_PAGE_SIZE - 1) /
            PageHead::NORMAL_PAGE_SIZE;
        const size_t order = ilog2(round_up_power_of_2(required_pages));

        PageHead* page = allocate_block(order);
        if (page != nullptr)
            page->m_size_if_huge_page = required_pages;
        return page;
    }

    void Chunk::free_page(PageHead* page)
    {
        assert(base_ != nullptr
            && page != nullptr
            && validate(page) != nullptr);

        size_t idx = page_to_index(page);
        uint8_t s = state_[idx].load(std::memory_order_acquire);

        if (!(s & STATE_ALLOCATED)) return;

        size_t order = s >> STATE_ORDER_SHIFT;
        size_t block_pages = size_t(1) << order;

        state_[idx].store(
            static_cast<uint8_t>(order << STATE_ORDER_SHIFT),
            std::memory_order_release);

        for (size_t j = 1; j < block_pages; ++j)
        {
            state_[idx + j].store(0, std::memory_order_release);
        }

        uint64_t old_head =
            free_lists_[order].load(std::memory_order_acquire);
        uint64_t new_head;
        do
        {
            links_[idx].store(old_head, std::memory_order_release);
            new_head = pack(
                static_cast<uint32_t>(idx),
                unpack_counter(old_head) + 1);
        } while (!free_lists_[order].compare_exchange_weak(
            old_head, new_head,
            std::memory_order_release,
            std::memory_order_acquire));
    }

    PageHead* Chunk::validate(void* ptr)
    {
        assert(base_ != nullptr);

        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t base_addr = reinterpret_cast<uintptr_t>(base_);

        if (addr < base_addr || addr >= base_addr + reserved_size_)
            return nullptr;

        size_t idx = (addr - base_addr) / PageHead::NORMAL_PAGE_SIZE;

        for (size_t order = 0; order <= max_order_; ++order)
        {
            size_t block_size = size_t(1) << order;
            size_t candidate = idx & ~(block_size - 1);
            uint8_t s = state_[candidate].load(std::memory_order_acquire);

            if ((s & STATE_ALLOCATED) &&
                (static_cast<size_t>(s >> STATE_ORDER_SHIFT) == order))
            {
                return index_to_page(candidate);
            }
        }

        return nullptr;
    }

    void Chunk::remove_from_free_list_defrag(
        size_t order, uint32_t target)
    {
        uint64_t prev = PACKED_NULL;
        uint64_t curr =
            free_lists_[order].load(std::memory_order_acquire);

        while (curr != PACKED_NULL)
        {
            uint32_t curr_idx = unpack_index(curr);
            if (curr_idx == target)
            {
                uint64_t next =
                    links_[curr_idx].load(std::memory_order_acquire);

                if (prev == PACKED_NULL)
                {
                    if (next == PACKED_NULL)
                    {
                        free_lists_[order].store(
                            PACKED_NULL, std::memory_order_release);
                    }
                    else
                    {
                        free_lists_[order].store(
                            pack(unpack_index(next),
                                unpack_counter(curr) + 1),
                            std::memory_order_release);
                    }
                }
                else
                {
                    links_[unpack_index(prev)].store(
                        next, std::memory_order_release);
                }
                links_[curr_idx].store(PACKED_NULL, std::memory_order_release);
                return;
            }
            prev = curr;
            curr = links_[curr_idx].load(std::memory_order_acquire);
        }
    }

    void Chunk::defragment()
    {
        assert(base_ != nullptr);

        std::lock_guard<std::mutex> lock(mutex_);

        for (size_t order = 0; order < max_order_; ++order)
        {
            size_t step = size_t(1) << (order + 1);
            size_t block_size = size_t(1) << order;

            for (size_t i = 0;
                i + block_size * 2 <= total_pages_;
                i += step)
            {
                size_t buddy = i + block_size;

                uint8_t s1 = state_[i].load(std::memory_order_relaxed);
                uint8_t s2 = state_[buddy].load(std::memory_order_relaxed);

                if ((s1 & STATE_ALLOCATED) || (s2 & STATE_ALLOCATED))
                    continue;
                if ((s1 >> STATE_ORDER_SHIFT) != order)
                    continue;
                if ((s2 >> STATE_ORDER_SHIFT) != order)
                    continue;

                remove_from_free_list_defrag(
                    order, static_cast<uint32_t>(i));
                remove_from_free_list_defrag(
                    order, static_cast<uint32_t>(buddy));

                state_[i].store(0, std::memory_order_relaxed);
                state_[buddy].store(0, std::memory_order_relaxed);

                state_[i].store(
                    static_cast<uint8_t>(
                        (order + 1) << STATE_ORDER_SHIFT),
                    std::memory_order_release);

                uint64_t old_head =
                    free_lists_[order + 1].load(std::memory_order_acquire);
                uint64_t new_head;
                do
                {
                    links_[i].store(old_head, std::memory_order_release);
                    new_head = pack(
                        static_cast<uint32_t>(i),
                        unpack_counter(old_head) + 1);
                } while (!free_lists_[order + 1].compare_exchange_weak(
                    old_head, new_head,
                    std::memory_order_release,
                    std::memory_order_acquire));
            }
        }
    }

    size_t Chunk::get_total_size() const
    {
        return reserved_size_;
    }

    size_t Chunk::get_total_page_count() const
    {
        return total_pages_;
    }
}
