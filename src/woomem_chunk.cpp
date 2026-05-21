#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_chunk.hpp"

#include <cstdlib>
#include <cstring>

namespace woomem
{

size_t Chunk::round_up_power_of_2(size_t v)
{
    if (v == 0)
        return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

size_t Chunk::ilog2(size_t v)
{
    size_t r = 0;
    while (v > 1)
    {
        v >>= 1;
        r++;
    }
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

Chunk::Chunk(size_t reserved_size)
{
    size_t pages = (reserved_size + Page::NORMAL_PAGE_SIZE - 1) / Page::NORMAL_PAGE_SIZE;
    total_pages_ = round_up_power_of_2(pages);
    reserved_size_ = total_pages_ * Page::NORMAL_PAGE_SIZE;
    max_order_ = ilog2(total_pages_);

    base_ = woomem_os_reserve_memory(reserved_size_);
    if (!base_)
    {
        total_pages_ = 0;
        reserved_size_ = 0;
        max_order_ = 0;
        state_ = nullptr;
        links_ = nullptr;
        free_lists_ = nullptr;
        return;
    }

    state_ = new std::atomic<uint8_t>[total_pages_];
    links_ = new std::atomic<uint64_t>[total_pages_];
    free_lists_ = new std::atomic<uint64_t>[max_order_ + 1];

    if (!state_ || !links_ || !free_lists_)
    {
        delete[] state_;
        delete[] links_;
        delete[] free_lists_;
        state_ = nullptr;
        links_ = nullptr;
        free_lists_ = nullptr;
        woomem_os_release_memory(base_, reserved_size_);
        base_ = nullptr;
        total_pages_ = 0;
        reserved_size_ = 0;
        max_order_ = 0;
        return;
    }

    for (size_t i = 0; i < total_pages_; i++)
        state_[i].store(0, std::memory_order_relaxed);

    for (size_t i = 0; i <= max_order_; i++)
        free_lists_[i].store(PACKED_NULL, std::memory_order_relaxed);

    state_[0].store(static_cast<uint8_t>(max_order_ << STATE_ORDER_SHIFT),
        std::memory_order_relaxed);
    links_[0].store(PACKED_NULL, std::memory_order_relaxed);
    free_lists_[max_order_].store(pack(0, 0), std::memory_order_relaxed);
}

Chunk::~Chunk()
{
    delete[] state_;
    delete[] links_;
    delete[] free_lists_;

    if (base_)
        woomem_os_release_memory(base_, reserved_size_);
}

size_t Chunk::page_to_index(Page* page) const
{
    return static_cast<size_t>(reinterpret_cast<char*>(page) - static_cast<char*>(base_))
        / Page::NORMAL_PAGE_SIZE;
}

Page* Chunk::index_to_page(size_t idx) const
{
    return reinterpret_cast<Page*>(static_cast<char*>(base_) + idx * Page::NORMAL_PAGE_SIZE);
}

size_t Chunk::addr_to_index(void* ptr) const
{
    return static_cast<size_t>(static_cast<char*>(ptr) - static_cast<char*>(base_))
        / Page::NORMAL_PAGE_SIZE;
}

void Chunk::remove_from_free_list_defrag(size_t order, uint32_t target)
{
    uint64_t prev_packed = PACKED_NULL;
    uint64_t curr_packed = free_lists_[order].load(std::memory_order_relaxed);

    while (curr_packed != PACKED_NULL)
    {
        uint32_t curr_idx = unpack_index(curr_packed);
        if (curr_idx == target)
        {
            uint64_t next = links_[target].load(std::memory_order_relaxed);
            if (prev_packed == PACKED_NULL)
                free_lists_[order].store(next, std::memory_order_relaxed);
            else
                links_[unpack_index(prev_packed)].store(next, std::memory_order_relaxed);
            return;
        }
        prev_packed = curr_packed;
        curr_packed = links_[curr_idx].load(std::memory_order_relaxed);
    }
}

Page* Chunk::allocate_block(size_t required_order)
{
    if (!base_ || required_order > max_order_)
        return nullptr;

    size_t block_pages = static_cast<size_t>(1) << required_order;
    size_t commit_size = block_pages * Page::NORMAL_PAGE_SIZE;

    for (;;)
    {
        size_t order = required_order;
        uint64_t packed = PACKED_NULL;

        while (order <= max_order_)
        {
            packed = free_lists_[order].load(std::memory_order_acquire);
            if (packed != PACKED_NULL)
                break;
            order++;
        }

        if (packed == PACKED_NULL)
            return nullptr;

        uint32_t idx = unpack_index(packed);
        uint64_t next = links_[idx].load(std::memory_order_acquire);

        if (!free_lists_[order].compare_exchange_weak(packed, next,
                std::memory_order_acquire, std::memory_order_relaxed))
        {
            continue;
        }

        void* commit_addr = static_cast<char*>(base_) + idx * Page::NORMAL_PAGE_SIZE;
        if (woomem_os_commit_memory(commit_addr, commit_size) != 0)
        {
            uint64_t head;
            do
            {
                head = free_lists_[order].load(std::memory_order_acquire);
                links_[idx].store(head, std::memory_order_release);
            } while (!free_lists_[order].compare_exchange_weak(head,
                    pack(idx, unpack_counter(head) + 1),
                    std::memory_order_release, std::memory_order_relaxed));
            return nullptr;
        }

        while (order > required_order)
        {
            order--;
            uint32_t buddy = idx | static_cast<uint32_t>(size_t(1) << order);

            state_[buddy].store(static_cast<uint8_t>(order << STATE_ORDER_SHIFT),
                std::memory_order_release);

            uint64_t head;
            do
            {
                head = free_lists_[order].load(std::memory_order_acquire);
                links_[buddy].store(head, std::memory_order_release);
            } while (!free_lists_[order].compare_exchange_weak(head,
                    pack(buddy, unpack_counter(head) + 1),
                    std::memory_order_release, std::memory_order_relaxed));
        }

        uint8_t alloc_state = static_cast<uint8_t>((required_order << STATE_ORDER_SHIFT) | STATE_ALLOCATED);
        for (size_t j = 0; j < block_pages; j++)
            state_[idx + j].store(alloc_state, std::memory_order_release);

        return index_to_page(idx);
    }
}

Page* Chunk::allocate_page()
{
    return allocate_block(0);
}

Page* Chunk::allocate_huge_page(size_t size)
{
    if (size == 0)
        return nullptr;

    size_t pages = (size + Page::NORMAL_PAGE_SIZE - 1) / Page::NORMAL_PAGE_SIZE;
    pages = round_up_power_of_2(pages);
    size_t order = ilog2(pages);
    return allocate_block(order);
}

void Chunk::free_page(Page* page)
{
    if (!base_ || !page)
        return;

    size_t idx = page_to_index(page);
    if (idx >= total_pages_)
        return;

    uint8_t st = state_[idx].load(std::memory_order_acquire);

    if (!(st & STATE_ALLOCATED))
        return;

    size_t order = st >> STATE_ORDER_SHIFT;
    size_t block_pages = static_cast<size_t>(1) << order;
    idx = idx & ~(block_pages - 1);

    uint8_t expected = st;
    uint8_t freed_state = static_cast<uint8_t>(order << STATE_ORDER_SHIFT);
    if (!state_[idx].compare_exchange_strong(expected, freed_state,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return;
    }

    size_t decommit_size = block_pages * Page::NORMAL_PAGE_SIZE;
    void* decommit_addr = static_cast<char*>(base_) + idx * Page::NORMAL_PAGE_SIZE;
    if (woomem_os_decommit_memory(decommit_addr, decommit_size) != 0)
    {
        state_[idx].store(st, std::memory_order_release);
        return;
    }

    for (size_t j = 1; j < block_pages; j++)
        state_[idx + j].store(0, std::memory_order_release);

    uint64_t head;
    do
    {
        head = free_lists_[order].load(std::memory_order_acquire);
        links_[idx].store(head, std::memory_order_release);
    } while (!free_lists_[order].compare_exchange_weak(head,
            pack(static_cast<uint32_t>(idx), unpack_counter(head) + 1),
            std::memory_order_release, std::memory_order_relaxed));
}

Page* Chunk::validate(void* ptr)
{
    if (!base_ || !ptr)
        return nullptr;

    ptrdiff_t diff = static_cast<char*>(ptr) - static_cast<char*>(base_);
    if (diff < 0 || static_cast<size_t>(diff) >= reserved_size_)
        return nullptr;

    size_t idx = addr_to_index(ptr);

    uint8_t st = state_[idx].load(std::memory_order_acquire);
    if (!(st & STATE_ALLOCATED))
        return nullptr;

    size_t order = st >> STATE_ORDER_SHIFT;
    size_t block_pages = static_cast<size_t>(1) << order;
    size_t start_idx = idx & ~(block_pages - 1);

    return index_to_page(start_idx);
}

void Chunk::defragment()
{
    if (!base_)
        return;

    for (size_t order = 0; order < max_order_; order++)
    {
        size_t stride = static_cast<size_t>(1) << (order + 1);
        for (size_t i = 0; i < total_pages_; i += stride)
        {
            uint32_t idx = static_cast<uint32_t>(i);
            uint32_t buddy = idx | static_cast<uint32_t>(size_t(1) << order);
            if (buddy >= total_pages_)
                continue;

            uint8_t st_a = state_[idx].load(std::memory_order_relaxed);
            if (st_a & STATE_ALLOCATED)
                continue;
            if ((st_a >> STATE_ORDER_SHIFT) != order)
                continue;

            uint8_t st_b = state_[buddy].load(std::memory_order_relaxed);
            if (st_b & STATE_ALLOCATED)
                continue;
            if ((st_b >> STATE_ORDER_SHIFT) != order)
                continue;

            if (idx != (idx & ~(stride - 1)))
                continue;

            remove_from_free_list_defrag(order, idx);
            remove_from_free_list_defrag(order, buddy);

            uint32_t merged_idx = (buddy < idx) ? buddy : idx;
            size_t new_order = order + 1;

            state_[merged_idx].store(
                static_cast<uint8_t>(new_order << STATE_ORDER_SHIFT),
                std::memory_order_relaxed);

            uint64_t head = free_lists_[new_order].load(std::memory_order_relaxed);
            links_[merged_idx].store(head, std::memory_order_relaxed);
            uint64_t new_packed = pack(merged_idx, unpack_counter(head) + 1);
            free_lists_[new_order].store(new_packed, std::memory_order_relaxed);
        }
    }
}

}
