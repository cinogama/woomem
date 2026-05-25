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
        , max_order_(0)
        , state_(nullptr)
        , prev_(nullptr)
        , next_(nullptr)
        , free_heads_(nullptr)
        , commit_(nullptr)
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
            return;
        }

        state_ = new uint8_t[total_pages_];
        prev_  = new uint32_t[total_pages_];
        next_  = new uint32_t[total_pages_];
        free_heads_ = new uint32_t[max_order_ + 1];
        commit_ = new uint8_t[total_pages_];

        memset(state_, 0, total_pages_);
        memset(commit_, 0, total_pages_);

        for (size_t i = 0; i < total_pages_; ++i)
        {
            prev_[i] = INDEX_NULL;
            next_[i] = INDEX_NULL;
        }

        for (size_t i = 0; i <= max_order_; ++i)
        {
            free_heads_[i] = INDEX_NULL;
        }

        state_[0] = static_cast<uint8_t>(max_order_ << STATE_ORDER_SHIFT);

        free_heads_[max_order_] = 0;
        prev_[0] = INDEX_NULL;
        next_[0] = INDEX_NULL;
    }

    Chunk::~Chunk()
    {
        delete[] state_;
        delete[] prev_;
        delete[] next_;
        delete[] free_heads_;
        delete[] commit_;
        if (base_)
        {
            woomem_os_release_memory(base_, reserved_size_);
        }
    }

    bool Chunk::is_init_failed() const
    {
        return base_ == nullptr;
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

    void Chunk::push_free(size_t order, uint32_t idx)
    {
        uint32_t head = free_heads_[order];
        free_heads_[order] = idx;
        prev_[idx] = INDEX_NULL;
        next_[idx] = head;
        if (head != INDEX_NULL)
        {
            prev_[head] = idx;
        }
    }

    uint32_t Chunk::pop_free(size_t order)
    {
        uint32_t head = free_heads_[order];
        if (head == INDEX_NULL)
            return INDEX_NULL;

        uint32_t succ = next_[head];
        free_heads_[order] = succ;
        if (succ != INDEX_NULL)
        {
            prev_[succ] = INDEX_NULL;
        }
        prev_[head] = INDEX_NULL;
        next_[head] = INDEX_NULL;
        return head;
    }

    void Chunk::remove_free(size_t order, uint32_t idx)
    {
        uint32_t p = prev_[idx];
        uint32_t n = next_[idx];

        if (p == INDEX_NULL)
        {
            free_heads_[order] = n;
        }
        else
        {
            next_[p] = n;
        }

        if (n != INDEX_NULL)
        {
            prev_[n] = p;
        }

        prev_[idx] = INDEX_NULL;
        next_[idx] = INDEX_NULL;
    }

    PageHead* Chunk::allocate_block(size_t order)
    {
        assert(base_ != nullptr);

        ReadWriteSpinlock::WriteGuard guard(rwlock_);

        for (size_t k = order; k <= max_order_; ++k)
        {
            if (free_heads_[k] == INDEX_NULL)
                continue;

            uint32_t idx = pop_free(k);

            while (k > order)
            {
                --k;
                size_t buddy = idx + (size_t(1) << k);
                state_[buddy] = static_cast<uint8_t>(k << STATE_ORDER_SHIFT);
                push_free(k, static_cast<uint32_t>(buddy));
            }

            const size_t block_pages = size_t(1) << order;

            PageHead* const page = commit_page(idx);
            page->m_page_just_allocated.store(
                true, std::memory_order_relaxed);

            state_[idx] = static_cast<uint8_t>(
                (order << STATE_ORDER_SHIFT) | STATE_ALLOCATED);

            for (size_t j = 1; j < block_pages; ++j)
            {
                (void)commit_page(idx + j);
                state_[idx + j] = STATE_CONTINUATION;
            }

            return page;
        }

        return nullptr;
    }

    PageHead* Chunk::allocate_page()
    {
        PageHead* page = allocate_block(0);
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
        const size_t order = ilog2(round_up_power_of_2(required_pages));

        PageHead* page = allocate_block(order);
        if (page != nullptr)
            page->m_page_count_if_huge = required_pages;
        return page;
    }

    void Chunk::free_page(PageHead* page)
    {
        assert(base_ != nullptr
            && page != nullptr
            && validate(page) != nullptr);

        ReadWriteSpinlock::WriteGuard guard(rwlock_);

        size_t idx = page_to_index(page);
        uint8_t s = state_[idx];

        if (!(s & STATE_ALLOCATED)) return;

        size_t order = s >> STATE_ORDER_SHIFT;
        size_t block_pages = size_t(1) << order;

        state_[idx] = static_cast<uint8_t>(order << STATE_ORDER_SHIFT);

        for (size_t j = 1; j < block_pages; ++j)
        {
            state_[idx + j] = 0;
        }

        while (order < max_order_)
        {
            size_t block_size = size_t(1) << order;
            size_t buddy = idx ^ block_size;

            if (buddy >= total_pages_)
                break;

            uint8_t bs = state_[buddy];
            if ((bs & STATE_ALLOCATED) != 0)
                break;
            if ((bs >> STATE_ORDER_SHIFT) != order)
                break;

            remove_free(order, static_cast<uint32_t>(buddy));

            state_[buddy] = 0;

            idx = idx < buddy ? idx : buddy;
            ++order;
        }

        state_[idx] = static_cast<uint8_t>(order << STATE_ORDER_SHIFT);
        push_free(order, static_cast<uint32_t>(idx));
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

        for (size_t order = 0; order <= max_order_; ++order)
        {
            size_t block_size = size_t(1) << order;
            size_t candidate = idx & ~(block_size - 1);
            uint8_t s = state_[candidate];

            if ((s & STATE_ALLOCATED) &&
                (static_cast<size_t>(s >> STATE_ORDER_SHIFT) == order))
            {
                return index_to_page(candidate);
            }
        }

        return nullptr;
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
