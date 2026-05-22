#include "woomem.h"
#include "woomem_os_mmap.h"

#include "woomem_chunk.hpp"

namespace woomem
{

void SpinLock::lock() noexcept
{
    while (flag_.exchange(1, std::memory_order_acquire))
    {
        while (flag_.load(std::memory_order_relaxed))
        {
#if defined(_MSC_VER)
            _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
        }
    }
}

void SpinLock::unlock() noexcept
{
    flag_.store(0, std::memory_order_release);
}

size_t Chunk::round_up_power_of_two(size_t n)
{
    if (n == 0) return 1;
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

int Chunk::ceil_log2(size_t n)
{
    if (n <= 1) return 0;
    int log = 0;
    size_t p = 1;
    while (p < n)
    {
        p <<= 1;
        log++;
    }
    return log;
}

Chunk::Chunk(size_t reserved_size)
{
    const size_t page_size = Page::NORMAL_PAGE_SIZE;

    if (reserved_size == 0)
        return;

    size_t aligned = (reserved_size + page_size - 1) & ~(page_size - 1);
    size_t need_pages = aligned / page_size;
    if (need_pages == 0) need_pages = 1;
    total_pages_ = round_up_power_of_two(need_pages);

    size_t total_bytes = total_pages_ * page_size;
    max_order_ = ceil_log2(total_pages_);

    pages_ = (Page*)woomem_os_reserve_memory(total_bytes);
    if (!pages_)
    {
        total_pages_ = 0;
        max_order_ = -1;
        return;
    }
    woomem_os_commit_memory(pages_, total_bytes);

    page_state_ = new uint16_t[total_pages_];
    free_heads_ = new size_t[max_order_ + 1];
    free_next_  = new size_t[total_pages_];

    for (size_t i = 0; i < total_pages_; i++)
    {
        page_state_[i] = 0;
        free_next_[i] = SIZE_MAX;
    }
    for (int i = 0; i <= max_order_; i++)
        free_heads_[i] = SIZE_MAX;

    page_state_[0] = FLAG_IS_HEAD | (uint16_t)max_order_;
    free_heads_[max_order_] = 0;
}

Chunk::~Chunk()
{
    if (pages_)
        woomem_os_release_memory(pages_, total_pages_ * Page::NORMAL_PAGE_SIZE);
    delete[] page_state_;
    delete[] free_heads_;
    delete[] free_next_;
}

Page* Chunk::allocate_page()
{
    if (max_order_ < 0) return nullptr;
    spinlock_.lock();
    size_t idx = alloc_block_(0);
    spinlock_.unlock();
    if (idx == SIZE_MAX) return nullptr;
    return &pages_[idx];
}

Page* Chunk::allocate_huge_page(size_t size)
{
    if (size == 0 || max_order_ < 0) return nullptr;

    size_t need_pages = (size + Page::NORMAL_PAGE_SIZE - 1) / Page::NORMAL_PAGE_SIZE;
    int order = ceil_log2(need_pages);
    if (order > max_order_) return nullptr;

    spinlock_.lock();
    size_t idx = alloc_block_(order);
    spinlock_.unlock();

    if (idx == SIZE_MAX) return nullptr;
    return &pages_[idx];
}

void Chunk::free_page(Page* page)
{
    if (!page || !pages_) return;

    uintptr_t base = (uintptr_t)pages_;
    uintptr_t addr = (uintptr_t)page;
    uintptr_t end  = base + (uintptr_t)total_pages_ * Page::NORMAL_PAGE_SIZE;

    spinlock_.lock();

    if (addr < base || addr >= end)
    {
        spinlock_.unlock();
        return;
    }

    size_t idx = (addr - base) / Page::NORMAL_PAGE_SIZE;
    uint16_t state = page_state_[idx];

    if (!(state & FLAG_ALLOCATED))
    {
        spinlock_.unlock();
        return;
    }

    size_t head_idx;
    int order;
    if (state & FLAG_IS_HEAD)
    {
        head_idx = idx;
        order = (int)(state & MASK_VALUE);
    }
    else
    {
        size_t back_off = state & MASK_VALUE;
        head_idx = idx - back_off;
        order = (int)(page_state_[head_idx] & MASK_VALUE);
    }

    free_block_(head_idx, order);

    spinlock_.unlock();
}

void Chunk::defragment()
{
}

Page* Chunk::validate(void* ptr)
{
    if (!ptr || !pages_) return nullptr;

    uintptr_t base = (uintptr_t)pages_;
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t end  = base + (uintptr_t)total_pages_ * Page::NORMAL_PAGE_SIZE;

    spinlock_.lock();

    if (addr < base || addr >= end)
    {
        spinlock_.unlock();
        return nullptr;
    }

    size_t idx = (addr - base) / Page::NORMAL_PAGE_SIZE;
    uint16_t state = page_state_[idx];

    if (!(state & FLAG_ALLOCATED))
    {
        spinlock_.unlock();
        return nullptr;
    }

    Page* result;
    if (state & FLAG_IS_HEAD)
    {
        result = &pages_[idx];
    }
    else
    {
        size_t back_off = state & MASK_VALUE;
        result = &pages_[idx - back_off];
    }

    spinlock_.unlock();
    return result;
}

size_t Chunk::alloc_block_(int order)
{
    int o = order;
    while (o <= max_order_ && free_heads_[o] == SIZE_MAX)
        o++;
    if (o > max_order_) return SIZE_MAX;

    size_t idx = free_heads_[o];
    free_heads_[o] = free_next_[idx];
    free_next_[idx] = SIZE_MAX;

    while (o > order)
    {
        o--;
        size_t buddy = idx + ((size_t)1 << o);
        size_t buddy_size = (size_t)1 << o;

        free_next_[buddy] = free_heads_[o];
        free_heads_[o] = buddy;

        page_state_[buddy] = FLAG_IS_HEAD | (uint16_t)o;
        for (size_t i = 1; i < buddy_size; i++)
            page_state_[buddy + i] = 0;
    }

    size_t block_size = (size_t)1 << order;
    page_state_[idx] = FLAG_ALLOCATED | FLAG_IS_HEAD | (uint16_t)order;
    for (size_t i = 1; i < block_size; i++)
        page_state_[idx + i] = FLAG_ALLOCATED | (uint16_t)i;

    return idx;
}

void Chunk::free_block_(size_t idx, int order)
{
    size_t block_size = (size_t)1 << order;

    page_state_[idx] = FLAG_IS_HEAD | (uint16_t)order;
    for (size_t i = 1; i < block_size; i++)
        page_state_[idx + i] = 0;

    while (order < max_order_)
    {
        size_t buddy = buddy_of_(idx, order);
        if (buddy >= total_pages_) break;

        uint16_t buddy_state = page_state_[buddy];
        if (buddy_state & FLAG_ALLOCATED) break;
        if (!(buddy_state & FLAG_IS_HEAD)) break;
        if ((buddy_state & MASK_VALUE) != (uint16_t)order) break;

        size_t prev = SIZE_MAX;
        size_t curr = free_heads_[order];
        while (curr != SIZE_MAX)
        {
            if (curr == buddy)
            {
                if (prev == SIZE_MAX)
                    free_heads_[order] = free_next_[buddy];
                else
                    free_next_[prev] = free_next_[buddy];
                break;
            }
            prev = curr;
            curr = free_next_[curr];
        }
        free_next_[buddy] = SIZE_MAX;

        idx = (idx < buddy) ? idx : buddy;
        order++;
        block_size <<= 1;

        page_state_[idx] = FLAG_IS_HEAD | (uint16_t)order;
        for (size_t i = 1; i < block_size; i++)
            page_state_[idx + i] = 0;
    }

    free_next_[idx] = free_heads_[order];
    free_heads_[order] = idx;
}

size_t Chunk::buddy_of_(size_t idx, int order) const
{
    return idx ^ ((size_t)1 << order);
}

}
