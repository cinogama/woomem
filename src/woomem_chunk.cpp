#include "woomem_os_mmap.h"

#include "woomem_chunk.hpp"

#include <cstdlib>
#include <cstring>
#include <cassert>

namespace woomem
{
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
        : base_(nullptr)
        , reserved_size_(0)
        , total_pages_(0)
        , count_(nullptr)
        , free_prev_(nullptr)
        , free_next_(nullptr)
        , free_head_(INDEX_NULL)
        , commit_(nullptr)
    {
        if (reserved_size == 0)
            return;

        total_pages_ =
            (reserved_size + PageHead::NORMAL_PAGE_SIZE - 1) /
            PageHead::NORMAL_PAGE_SIZE;
        reserved_size_ = total_pages_ * PageHead::NORMAL_PAGE_SIZE;

        base_ = woomem_os_reserve_memory(reserved_size_);
        if (!base_)
        {
            total_pages_ = 0;
            reserved_size_ = 0;
            return;
        }

        count_      = new uint32_t[total_pages_]();
        free_prev_  = new uint32_t[total_pages_];
        free_next_  = new uint32_t[total_pages_];
        commit_     = new uint8_t[total_pages_]();

        for (size_t i = 0; i < total_pages_; ++i)
        {
            free_prev_[i] = INDEX_NULL;
            free_next_[i] = INDEX_NULL;
        }

        count_[0] = static_cast<uint32_t>(total_pages_);
        free_head_ = 0;
    }

    Chunk::~Chunk()
    {
        delete[] count_;
        delete[] free_prev_;
        delete[] free_next_;
        delete[] commit_;
        if (base_)
        {
            woomem_os_release_memory(base_, reserved_size_);
        }
    }

    bool Chunk::is_init_failed() const
    {
        return base_ == nullptr && total_pages_ == 0;
    }

    PageHead* Chunk::commit_page(size_t idx)
    {
        PageHead* const p = index_to_page(idx);
        if (!commit_[idx])
        {
            woomem_os_commit_memory(p, PageHead::NORMAL_PAGE_SIZE);
            commit_[idx] = 1;
        }
        return p;
    }

    void Chunk::free_list_remove(uint32_t idx)
    {
        uint32_t prev = free_prev_[idx];
        uint32_t next = free_next_[idx];

        if (prev != INDEX_NULL)
            free_next_[prev] = next;
        else
            free_head_ = next;

        if (next != INDEX_NULL)
            free_prev_[next] = prev;

        free_prev_[idx] = INDEX_NULL;
        free_next_[idx] = INDEX_NULL;
    }

    void Chunk::free_list_insert(uint32_t idx, uint32_t count)
    {
    restart:
        uint32_t prev = INDEX_NULL;
        uint32_t next = free_head_;
        while (next != INDEX_NULL && next < idx)
        {
            prev = next;
            next = free_next_[next];
        }

        if (prev != INDEX_NULL)
        {
            uint32_t prev_count = count_[prev];
            if (prev + prev_count == idx)
            {
                free_list_remove(prev);
                count = prev_count + count;
                idx = prev;
                goto restart;
            }
        }

        if (next != INDEX_NULL)
        {
            uint32_t next_count = count_[next];
            if (idx + count == next)
            {
                free_list_remove(next);
                count = count + next_count;
                goto restart;
            }
        }

        free_prev_[idx] = prev;
        free_next_[idx] = next;

        if (prev != INDEX_NULL)
            free_next_[prev] = idx;
        else
            free_head_ = idx;

        if (next != INDEX_NULL)
            free_prev_[next] = idx;

        count_[idx] = count;
    }

    uint32_t Chunk::free_list_find_block(uint32_t required) const
    {
        uint32_t curr = free_head_;
        while (curr != INDEX_NULL)
        {
            if ((count_[curr] & COUNT_MASK) >= required)
                return curr;
            curr = free_next_[curr];
        }
        return INDEX_NULL;
    }

    PageHead* Chunk::allocate_pages(uint32_t required_pages)
    {
        ReadWriteSpinlock::WriteGuard guard(rwlock_);

        uint32_t idx = free_list_find_block(required_pages);
        if (idx == INDEX_NULL)
            return nullptr;

        uint32_t block_count = count_[idx] & COUNT_MASK;

        free_list_remove(idx);

        if (block_count > required_pages)
        {
            uint32_t left_idx = idx + required_pages;
            uint32_t left_count = block_count - required_pages;

            count_[left_idx] = left_count;
            for (uint32_t j = 1; j < left_count; ++j)
                count_[left_idx + j] = 0;

            free_list_insert(left_idx, left_count);
        }

        count_[idx] = required_pages | ALLOCATED_FLAG;
        for (uint32_t j = 1; j < required_pages; ++j)
            count_[idx + j] = 0;

        for (uint32_t j = 0; j < required_pages; ++j)
            (void)commit_page(idx + j);

        PageHead* const page = index_to_page(idx);
        page->m_page_just_allocated.store(
            true, std::memory_order_relaxed);
        return page;
    }

    PageHead* Chunk::allocate_page()
    {
        PageHead* page = allocate_pages(1);
        if (page != nullptr)
            page->m_page_count_if_huge = 0;
        return page;
    }

    PageHead* Chunk::allocate_huge_page(size_t size)
    {
        assert(base_ != nullptr && size != 0);

        const size_t required_pages =
            (size + PageHead::NORMAL_PAGE_SIZE - 1) /
            PageHead::NORMAL_PAGE_SIZE;

        if (required_pages > total_pages_)
            return nullptr;

        PageHead* page = allocate_pages(static_cast<uint32_t>(required_pages));
        if (page != nullptr)
            page->m_page_count_if_huge = required_pages;
        return page;
    }

    void Chunk::free_page(PageHead* page)
    {
        assert(base_ != nullptr
            && page != nullptr
            && validate(page) == page);

        ReadWriteSpinlock::WriteGuard guard(rwlock_);

        size_t idx = page_to_index(page);
        uint32_t c = count_[idx];

        if (!(c & ALLOCATED_FLAG))
            return;

        uint32_t block_count = c & COUNT_MASK;

        for (uint32_t j = 0; j < block_count; ++j)
            count_[idx + j] = 0;

        count_[idx] = block_count;

        free_list_insert(static_cast<uint32_t>(idx), block_count);
    }

    PageHead* Chunk::validate(void* ptr)
    {
        assert(base_ != nullptr);

        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t base_addr = reinterpret_cast<uintptr_t>(base_);

        if (addr < base_addr || addr >= base_addr + reserved_size_)
            return nullptr;

        size_t idx = (addr - base_addr) / PageHead::NORMAL_PAGE_SIZE;

        ReadWriteSpinlock::ReadGuard guard(rwlock_);

        size_t head_idx = idx;
        while (count_[head_idx] == 0)
        {
            if (head_idx == 0)
                return nullptr;
            --head_idx;
        }

        uint32_t c = count_[head_idx];

        if ((c & ALLOCATED_FLAG) == 0)
            return nullptr;

        uint32_t block_count = c & COUNT_MASK;
        if (idx >= head_idx + block_count)
            return nullptr;

        return index_to_page(head_idx);
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
