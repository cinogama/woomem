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

    state_ = static_cast<uint8_t*>(malloc(total_pages_ * sizeof(uint8_t)));
    links_ = static_cast<uint32_t*>(malloc(total_pages_ * sizeof(uint32_t)));
    free_lists_ = static_cast<uint32_t*>(malloc((max_order_ + 1) * sizeof(uint32_t)));

    if (!state_ || !links_ || !free_lists_)
    {
        free(state_);
        free(links_);
        free(free_lists_);
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

    memset(state_, 0, total_pages_ * sizeof(uint8_t));

    for (size_t i = 0; i <= max_order_; i++)
        free_lists_[i] = FREE_LIST_NULL;

    state_[0] = static_cast<uint8_t>(max_order_ << STATE_ORDER_SHIFT);
    links_[0] = FREE_LIST_NULL;
    free_lists_[max_order_] = 0;
}

Chunk::~Chunk()
{
    free(state_);
    free(links_);
    free(free_lists_);

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

void Chunk::remove_from_free_list(size_t order, uint32_t target)
{
    uint32_t* ref = &free_lists_[order];
    while (*ref != FREE_LIST_NULL)
    {
        if (*ref == target)
        {
            *ref = links_[target];
            return;
        }
        ref = &links_[*ref];
    }
}

Page* Chunk::allocate_block(size_t required_order)
{
    if (!base_ || required_order > max_order_)
        return nullptr;

    size_t order = required_order;
    while (order <= max_order_ && free_lists_[order] == FREE_LIST_NULL)
        order++;

    if (order > max_order_)
        return nullptr;

    uint32_t idx = free_lists_[order];

    size_t block_pages = static_cast<size_t>(1) << required_order;
    size_t commit_size = block_pages * Page::NORMAL_PAGE_SIZE;
    void* commit_addr = static_cast<char*>(base_) + idx * Page::NORMAL_PAGE_SIZE;
    if (woomem_os_commit_memory(commit_addr, commit_size) != 0)
        return nullptr;

    free_lists_[order] = links_[idx];

    while (order > required_order)
    {
        order--;
        uint32_t buddy = idx | static_cast<uint32_t>(size_t(1) << order);

        state_[buddy] = static_cast<uint8_t>(order << STATE_ORDER_SHIFT);
        links_[buddy] = free_lists_[order];
        free_lists_[order] = buddy;
    }

    uint8_t alloc_state = static_cast<uint8_t>((order << STATE_ORDER_SHIFT) | STATE_ALLOCATED);
    for (size_t j = 0; j < block_pages; j++)
        state_[idx + j] = alloc_state;

    return index_to_page(idx);
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
    uint8_t st = state_[idx];

    if (!(st & STATE_ALLOCATED))
        return;

    size_t order = st >> STATE_ORDER_SHIFT;
    size_t block_pages = static_cast<size_t>(1) << order;
    idx = idx & ~(block_pages - 1);

    size_t decommit_size = block_pages * Page::NORMAL_PAGE_SIZE;
    void* decommit_addr = static_cast<char*>(base_) + idx * Page::NORMAL_PAGE_SIZE;
    if (woomem_os_decommit_memory(decommit_addr, decommit_size) != 0)
        return;

    for (size_t j = 0; j < block_pages; j++)
        state_[idx + j] = 0;

    state_[idx] = static_cast<uint8_t>(order << STATE_ORDER_SHIFT);

    while (true)
    {
        uint32_t buddy = static_cast<uint32_t>(idx) ^ static_cast<uint32_t>(size_t(1) << order);

        if (buddy >= total_pages_)
            break;

        uint8_t buddy_st = state_[buddy];
        if ((buddy_st & STATE_ALLOCATED) || ((buddy_st >> STATE_ORDER_SHIFT) != order))
            break;

        remove_from_free_list(order, buddy);

        if (buddy < idx)
            idx = buddy;

        order++;
        state_[idx] = static_cast<uint8_t>(order << STATE_ORDER_SHIFT);
    }

    links_[idx] = free_lists_[order];
    free_lists_[order] = static_cast<uint32_t>(idx);
}

Page* Chunk::validate(void* ptr)
{
    if (!base_ || !ptr)
        return nullptr;

    ptrdiff_t diff = static_cast<char*>(ptr) - static_cast<char*>(base_);
    if (diff < 0 || static_cast<size_t>(diff) >= reserved_size_)
        return nullptr;

    size_t idx = addr_to_index(ptr);

    uint8_t st = state_[idx];
    if (!(st & STATE_ALLOCATED))
        return nullptr;

    size_t order = st >> STATE_ORDER_SHIFT;
    size_t block_pages = static_cast<size_t>(1) << order;
    size_t start_idx = idx & ~(block_pages - 1);

    return index_to_page(start_idx);
}

}
