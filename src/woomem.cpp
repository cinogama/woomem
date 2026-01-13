#include "woomem.h"
#include "woomem_os_mmap.h"

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <atomic>
#include <new>
#include <vector>
#include <algorithm>

using namespace std;

#if defined(__GNUC__) || defined(__clang__)
#   define WOOMEM_LIKELY(x)       __builtin_expect(!!(x), 1)
#   define WOOMEM_UNLIKELY(x)     __builtin_expect(!!(x), 0)
#   define WOOMEM_FORCE_INLINE    __attribute__((always_inline)) inline
#   define WOOMEM_NOINLINE        __attribute__((noinline))
#   if defined(__aarch64__) || defined(_M_ARM64)
#       define WOOMEM_PAUSE()   __asm__ __volatile__("yield");
#   elif defined(__x86_64__) || defined(_M_X64)
#       define WOOMEM_PAUSE()   __asm__ __volatile__("pause");
#   endif
#elif defined(_MSC_VER)
#   include <intrin.h>
#   define WOOMEM_LIKELY(x)       (x)
#   define WOOMEM_UNLIKELY(x)     (x)
#   define WOOMEM_FORCE_INLINE    __forceinline
#   define WOOMEM_NOINLINE        __declspec(noinline)
#   if defined(_M_ARM64) || defined(__aarch64__)
#       define WOOMEM_PAUSE()       __yield();
#   elif defined(_M_X64) || defined(__x86_64__)
#       define WOOMEM_PAUSE()       _mm_pause();
#   endif
#else
#   define WOOMEM_LIKELY(x)       (x)
#   define WOOMEM_UNLIKELY(x)     (x)
#   define WOOMEM_FORCE_INLINE    inline
#   define WOOMEM_NOINLINE
#endif

#ifndef WOOMEM_PAUSE
#   define WOOMEM_PAUSE()       ((void)0)
#endif

namespace woomem_cppimpl
{
    constexpr size_t PAGE_SIZE = 64 * 1024;             // 64KB

    /* A Chunk will store many page, each page will be used for One specified allocated size */
    constexpr size_t CHUNK_SIZE = 128 * 1024 * 1024;    // 128MB

    constexpr size_t CARDTABLE_SIZE_PER_BIT = 512;      // 512 Bytes per bit

    constexpr size_t BASE_ALIGNMENT = 8;                // 8 Bytes

    constexpr uint8_t NEW_BORN_GC_AGE = 15;

    enum PageGroupType : uint8_t
    {
        SMALL_8,
        SMALL_24,
        SMALL_40,
        SMALL_56,
        SMALL_88,
        SMALL_128,
        SMALL_192,
        SMALL_264,
        SMALL_344,
        SMALL_488,
        SMALL_704,
        SMALL_920,
        SMALL_1024,

        MIDIUM_1440,
        MIDIUM_2168,
        MIDIUM_3104,
        MIDIUM_4352,
        MIDIUM_6536,
        MIDIUM_9344,
        MIDIUM_13088,
        MIDIUM_21824,
        MIDIUM_32744,
        MIDIUM_65504,

        LARGE,

        TOTAL_GROUP_COUNT = LARGE,
    };
    constexpr size_t UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[] =
    {
        // Small page groups
        8, 24, 40, 56, 88, 128, 192, 264, 344, 488, 704, 920, 1024,

        // Medium page groups
        1440, 2168, 3104, 4352, 6536, 9344, 13088, 21824, 32744, 65504,
    };
    static_assert(UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[PageGroupType::SMALL_1024] == 1024,
        "UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP must be correct.");
    static_assert(UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[PageGroupType::MIDIUM_65504] == 65504,
        "UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP must be correct.");

    constexpr size_t MAX_SMALL_UNIT_SIZE = 1024;
    constexpr size_t SMALL_UNIT_FAST_LOOKUP_TABLE_SIZE =
        (MAX_SMALL_UNIT_SIZE + 7) / 8 + 1;

    // 优化：将查找表按 cache line 对齐，减少 cache miss
    constexpr PageGroupType SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[SMALL_UNIT_FAST_LOOKUP_TABLE_SIZE] =
    {
        SMALL_8,
        SMALL_8,
        SMALL_24,
        SMALL_24,
        SMALL_40,
        SMALL_40,
        SMALL_56,
        SMALL_56,
        SMALL_88,
        SMALL_88,
        SMALL_88,
        SMALL_88,
        SMALL_128,
        SMALL_128,
        SMALL_128,
        SMALL_128,
        SMALL_128,
        SMALL_192,
        SMALL_192,
        SMALL_192,
        SMALL_192,
        SMALL_192,
        SMALL_192,
        SMALL_192,
        SMALL_192,
        SMALL_264,
        SMALL_264,
        SMALL_264,
        SMALL_264,
        SMALL_264,
        SMALL_264,
        SMALL_264,
        SMALL_264,
        SMALL_264,
        SMALL_344,
        SMALL_344,
        SMALL_344,
        SMALL_344,
        SMALL_344,
        SMALL_344,
        SMALL_344,
        SMALL_344,
        SMALL_344,
        SMALL_344,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_488,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_704,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_920,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
        SMALL_1024,
    };
    static_assert(sizeof(SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B) == SMALL_UNIT_FAST_LOOKUP_TABLE_SIZE,
        "SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B used for alloc(<= MAX_SMALL_UNIT_SIZE).");
    static_assert(SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[SMALL_UNIT_FAST_LOOKUP_TABLE_SIZE - 1] == SMALL_1024,
        "SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B must be filled correctly.");

    // 优化：预先计算常用大小的分配组，减少分支和计算
    // 针对 benchmark 中常用的 64 字节进行特化
    WOOMEM_FORCE_INLINE PageGroupType get_page_group_type_for_size(size_t size) noexcept
    {
        // 快速路径：小于等于 1024 字节使用查找表
        // 使用无分支的方式：先计算索引，再检查边界
        const size_t lookup_index = (size + 7) >> 3;

        if (WOOMEM_LIKELY(lookup_index < SMALL_UNIT_FAST_LOOKUP_TABLE_SIZE))
        {
            return SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[lookup_index];
        }

        // 中等大小：使用展开的比较链代替二分查找（减少分支预测失败）
        if (size <= 1440) return MIDIUM_1440;
        if (size <= 2168) return MIDIUM_2168;
        if (size <= 3104) return MIDIUM_3104;
        if (size <= 4352) return MIDIUM_4352;
        if (size <= 6536) return MIDIUM_6536;
        if (size <= 9344) return MIDIUM_9344;
        if (size <= 13088) return MIDIUM_13088;
        if (size <= 21824) return MIDIUM_21824;
        if (size <= 32744) return MIDIUM_32744;
        if (size <= 65504) return MIDIUM_65504;

        return LARGE;
    }

    union Page;
    struct PageHead
    {
        Page* m_next_page;

        PageGroupType m_page_belong_to_group;

        // m_free_times 将在 m_next_alloc_unit_head_offset 耗尽之后重设，
        // 然后检查 m_freed_unit_head_offset，如果页面彻底耗尽，没有空余
        // 单元，此页面将被抛弃，TLS Pool 将尝试重新拉取一个新的 Page
        // 
        // m_abondon_page_flag 将在页面被抛弃时设置
        //
        // 将统计被抛弃的页面总数，达到一定值时，GC 将把可以复用的页面重新
        // 拉起。
        atomic_uint8_t  m_abondon_page_flag;
        atomic_uint16_t m_free_times;

        atomic_uint16_t m_freed_unit_head_offset;
        uint16_t m_next_alloc_unit_head_offset;
    };
    static_assert(sizeof(PageHead) == 16 && alignof(PageHead) == 8,
        "PageHead size and alignment must be correct.");

    struct UnitHead
    {
        /* Used for user free. */
        Page* m_parent_page;

        atomic_uint8_t  m_allocated_status; // 0 = freed, 1 = allocated

        uint8_t         m_alloc_timing : 4;
        uint8_t /* woomem_GCUnitType */
            m_gc_type : 4;
        uint8_t         m_gc_age;
        atomic_uint8_t  m_gc_marked;

        uint16_t m_next_alloc_unit_offset;

        WOOMEM_FORCE_INLINE bool try_free_this_unit_head() noexcept
        {
            if (WOOMEM_LIKELY(m_allocated_status.exchange(0, std::memory_order_relaxed)))
            {
                // 延迟 GC 字段重置到下次分配时
                return true;
            }
            return false;
        }
    };
    static_assert(sizeof(UnitHead) == 16 && alignof(UnitHead) == 8,
        "UnitHead size and alignment must be correct.");

    union Page
    {
        PageHead m_page_head;
        char m_entries[PAGE_SIZE];

        void reinit_page_with_group(PageGroupType group_type) noexcept
        {
            // Only empty and new page can be reinit.
            assert(group_type != PageGroupType::LARGE);

            m_page_head.m_next_page = nullptr;
            m_page_head.m_page_belong_to_group = group_type;
            m_page_head.m_abondon_page_flag.store(0, std::memory_order_relaxed);
            m_page_head.m_free_times.store(0, std::memory_order_relaxed);
            m_page_head.m_freed_unit_head_offset.store(0, std::memory_order_relaxed);

            const size_t unit_take_size_unit =
                UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[group_type] + sizeof(UnitHead);

            uint16_t* next_unit_offset_ptr =
                &m_page_head.m_next_alloc_unit_head_offset;

            for (size_t current_unit_head_begin = sizeof(PageHead);
                current_unit_head_begin + unit_take_size_unit <= PAGE_SIZE;
                current_unit_head_begin += unit_take_size_unit)
            {
                *next_unit_offset_ptr = static_cast<uint16_t>(current_unit_head_begin);

                // Init unit head here.
                UnitHead* unit_head =
                    reinterpret_cast<UnitHead*>(&m_entries[current_unit_head_begin]);

                unit_head->m_parent_page = this;
                unit_head->m_allocated_status.store(0, std::memory_order_relaxed);

                /* unit_head->m_alloc_timing will be set when allocate. */
                unit_head->m_gc_age = NEW_BORN_GC_AGE;
                unit_head->m_gc_marked.store(
                    WOOMEM_GC_MARKED_UNMARKED, std::memory_order_relaxed);

                next_unit_offset_ptr = &unit_head->m_next_alloc_unit_offset;
            }
            *next_unit_offset_ptr = 0; // End of units.
        }

        /*
        ATTENTION: This page must belong current thread's TlsPageCollection.
        */
        /* OPTIONAL */ UnitHead* try_allocate_unit_from_page() noexcept
        {
            const auto next_alloc_unit_head_offset =
                m_page_head.m_next_alloc_unit_head_offset;

            if (WOOMEM_LIKELY(next_alloc_unit_head_offset))
            {
                UnitHead* const allocating_unit_head =
                    reinterpret_cast<UnitHead*>(&m_entries[next_alloc_unit_head_offset]);

                assert(0 == allocating_unit_head->m_allocated_status.load(
                    std::memory_order_relaxed));

                /*
                ATTENTION: Attribute and allocated flag will be set after
                        this function returns.
                */
                m_page_head.m_next_alloc_unit_head_offset =
                    allocating_unit_head->m_next_alloc_unit_offset;

                assert(m_page_head.m_next_alloc_unit_head_offset % 8 == 0);

                return allocating_unit_head;
            }
            return nullptr;
        }

        /*
        ATTENTION: This page must belong current thread's TlsPageCollection.
        */
        bool try_tidy_page() noexcept
        {
            if (m_page_head.m_next_alloc_unit_head_offset != 0)
                // Still have unit to alloc.
                return true;

            // Reset free times count.
            // NOTE: m_free_times must happend before m_freed_unit_head_offset.
            //      or m_free_time might missing count.
            if (0 != m_page_head.m_free_times.exchange(0, std::memory_order_acq_rel))
            {
                // Make sure `m_page_head` store happend before load of `m_freed_unit_head_offset`.
                // NOTE: m_freed_unit_head_offset might be modified by other threads.
                //      we need to exchange it with 0 first.
                const uint16_t free_list = m_page_head.m_freed_unit_head_offset.exchange(
                    0, std::memory_order_acq_rel);

                assert(free_list != 0);

                m_page_head.m_next_alloc_unit_head_offset = free_list;
                return true;
            }
            return false;
        }
        void drop_back_unit_in_this_page_asyncly(UnitHead* freeing_unit_head) noexcept
        {
            assert(freeing_unit_head->m_parent_page == this);
            assert(0 == freeing_unit_head->m_allocated_status.load(std::memory_order_relaxed));

            freeing_unit_head->m_next_alloc_unit_offset =
                m_page_head.m_freed_unit_head_offset.load(
                    std::memory_order_relaxed);

            // Ok, this unit is freed by current thread now
            const uint16_t current_unit_offset =
                static_cast<uint16_t>(
                    reinterpret_cast<char*>(freeing_unit_head) -
                    m_entries);

            assert(current_unit_offset % 8 == 0);

            while (
                !m_page_head.m_freed_unit_head_offset.compare_exchange_weak(
                    freeing_unit_head->m_next_alloc_unit_offset,
                    current_unit_offset,
                    std::memory_order_release,
                    std::memory_order_relaxed))
            {
                WOOMEM_PAUSE();
            }

            // Count for page reuses.
            // NOTE: Use release order to make sure count operation always happend 
            //      after the free operation.
            (void)m_page_head.m_free_times.fetch_add(1, std::memory_order_release);
        }
        void free_unit_in_this_page_asyncly(UnitHead* freeing_unit_head) noexcept
        {
            assert(freeing_unit_head->m_parent_page == this);

            if (freeing_unit_head->try_free_this_unit_head())
            {
                drop_back_unit_in_this_page_asyncly(freeing_unit_head);
            }
        }

        static void free_page_unit_asyncly(void* valid_page_unit)
        {
            UnitHead* unit_head =
                reinterpret_cast<UnitHead*>(
                    reinterpret_cast<char*>(valid_page_unit) - sizeof(UnitHead));

            unit_head->m_parent_page->free_unit_in_this_page_asyncly(unit_head);
        }
    };
    static_assert(sizeof(Page) == PAGE_SIZE,
        "Page size must be equal to PAGE_SIZE");

    struct Chunk
    {
        static atomic<Chunk*> g_current_chunk;

        Chunk* m_last_chunk;

        std::atomic_size_t m_next_commiting_page_count;
        std::atomic_size_t m_commited_page_count;

        Page* const m_reserved_address_begin;
        Page* const m_reserved_address_end;
        std::atomic_uint64_t m_cardtable[CHUNK_SIZE / CARDTABLE_SIZE_PER_BIT / 64];

        static_assert(std::atomic_uint64_t::is_always_lock_free,
            "atomic_uint64_t must be lock free for performance");

        Chunk(void* reserved_address) noexcept
            : m_last_chunk(nullptr)
            , m_next_commiting_page_count{ 0 }
            , m_commited_page_count{ 0 }
            , m_reserved_address_begin(reinterpret_cast<Page*>(reserved_address))
            , m_reserved_address_end(reinterpret_cast<Page*>(
                reinterpret_cast<uint8_t*>(reserved_address) + CHUNK_SIZE))
            , m_cardtable{}
        {
            assert(reserved_address != nullptr);
        }

        Chunk(const Chunk&) = delete;
        Chunk(Chunk&&) = delete;
        Chunk& operator=(const Chunk&) = delete;
        Chunk& operator=(Chunk&&) = delete;

        ~Chunk()
        {
            // Todo.
            const size_t commited_page_count =
                m_commited_page_count.load(std::memory_order_relaxed);

            for (size_t i = 0; i < commited_page_count; ++i)
            {
                const int decommit_status = woomem_os_decommit_memory(
                    &m_reserved_address_begin[i],
                    PAGE_SIZE);
                assert(decommit_status == 0);
                (void)decommit_status;
            }

            const int release_status = woomem_os_release_memory(
                m_reserved_address_begin,
                CHUNK_SIZE);
            assert(release_status == 0);
            (void)release_status;
        }

        /* OPTIONAL */ Page* allocate_new_page_in_chunk(
            PageGroupType page_group, bool* out_page_run_out) noexcept
        {
            size_t new_page_index =
                m_next_commiting_page_count.fetch_add(1, std::memory_order_relaxed);

            if (new_page_index < CHUNK_SIZE / PAGE_SIZE)
            {
                *out_page_run_out = false;

                Page* new_alloc_page = &m_reserved_address_begin[new_page_index];
                const auto status = woomem_os_commit_memory(
                    new_alloc_page,
                    PAGE_SIZE);

                if (status == 0)
                {
                    // Init this page.
                    new_alloc_page->reinit_page_with_group(page_group);

                    // Wait until other threads finish committing.
                    do
                    {
                        size_t expected_commited = new_page_index;
                        if (m_commited_page_count.compare_exchange_weak(
                            expected_commited,
                            new_page_index + 1,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed))
                        {
                            // Successfully updated commited_page_count
                            return &m_reserved_address_begin[new_page_index];
                        }
                        else if (m_next_commiting_page_count.load(std::memory_order_relaxed) <= new_page_index)
                        {
                            // Another thread has rollbacked.
                            const int decommit_status = woomem_os_decommit_memory(
                                &m_reserved_address_begin[new_page_index],
                                PAGE_SIZE);

                            assert(decommit_status == 0);
                            (void)decommit_status;

                            return nullptr;
                        }

                    } while (true);

                    // Should not reach here.
                    abort();
                }
                else
                {
                    // Failed to commit, rollback.
                    do
                    {
                        size_t current_commiting_page_count =
                            m_next_commiting_page_count.load(
                                std::memory_order_relaxed);

                        if (m_next_commiting_page_count > new_page_index)
                        {
                            // Need rollback.
                            if (m_next_commiting_page_count.compare_exchange_weak(
                                current_commiting_page_count,
                                new_page_index,
                                std::memory_order_relaxed))
                            {
                                // Already rollbacked by this thread.
                                return nullptr;
                            }
                            else if (current_commiting_page_count <= new_page_index)
                            {
                                // Another thread has rollbacked.
                                return nullptr;
                            }
                        }
                        else
                            // Already rollbacked by other thread.
                            return nullptr;

                    } while (true);
                }
            }
            else
            {
                *out_page_run_out = true;

                // Donot need rollback, just return nullptr.
                return nullptr;
            }
        }

        static /* OPTIONAL */ Chunk* create_new_chunk()
        {
            // Need to allocate a new chunk.
            void* chunk_storage = malloc(sizeof(Chunk));
            if (WOOMEM_UNLIKELY(chunk_storage == nullptr))
            {
                // No more memory !!!
                return nullptr;
            }

            void* reserved_address = woomem_os_reserve_memory(CHUNK_SIZE);
            if (WOOMEM_UNLIKELY(reserved_address == nullptr))
            {
                // Cannot reserve virtual address for new chunk.
                free(chunk_storage);
                return nullptr;
            }

            Chunk* new_chunk = new (chunk_storage) Chunk(reserved_address);
            new_chunk->m_last_chunk = g_current_chunk.load(std::memory_order_acquire);

            while (!g_current_chunk.compare_exchange_weak(
                new_chunk->m_last_chunk,
                new_chunk,
                std::memory_order_release,
                std::memory_order_acquire))
            {
                WOOMEM_PAUSE();
            }
            return new_chunk;
        }

        static /* OPTIONAL */ Page* allocate_new_page(PageGroupType page_group)
        {
            Chunk* current_chunk =
                g_current_chunk.load(std::memory_order_relaxed);

            do
            {
                bool page_run_out;
                Page* new_page = current_chunk->allocate_new_page_in_chunk(
                    page_group, &page_run_out);

                if (WOOMEM_UNLIKELY(page_run_out))
                {
                    // Check last chunk?
                    current_chunk = current_chunk->m_last_chunk;

                    if (WOOMEM_UNLIKELY(current_chunk == nullptr))
                    {
                        current_chunk = create_new_chunk();

                        if (WOOMEM_UNLIKELY(current_chunk == nullptr))
                            // Failed to alloc chunk..
                            return nullptr;
                    }
                    continue;
                }
                // new_page might be nullptr if commit memory failed.
                return new_page;

            } while (true);

            // Never reach here.
            abort();
        }
    };

    struct GlobalPageCollection
    {
        atomic<Page*> m_free_group_page_list[TOTAL_GROUP_COUNT];

        /* OPTIONAL */ Page* try_get_free_page(PageGroupType group_type) noexcept
        {
            auto& group = m_free_group_page_list[group_type];

            auto* free_page = group.load(std::memory_order_acquire);

            while (free_page != nullptr)
            {
                if (group.compare_exchange_weak(
                    free_page,
                    free_page->m_page_head.m_next_page,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                {
                    // Successfully got the page
                    free_page->m_page_head.m_next_page = nullptr;
                    return free_page;
                }
                // CAS failed, free_page is updated to current value, retry
                WOOMEM_PAUSE();
            }

            // No free page in pool, allocate a new one from Chunk
            return Chunk::allocate_new_page(group_type);
        }
        void return_free_page(Page* freeing_page) noexcept
        {
            auto& group = m_free_group_page_list[
                freeing_page->m_page_head.m_page_belong_to_group];

            freeing_page->m_page_head.m_next_page =
                group.load(std::memory_order_relaxed);

            while (!group.compare_exchange_weak(
                freeing_page->m_page_head.m_next_page,
                freeing_page,
                std::memory_order_release,
                std::memory_order_relaxed))
            {
                WOOMEM_PAUSE();
            }
        }
    };
    GlobalPageCollection g_global_page_collection;

    constexpr size_t THREAD_LOCAL_POOL_MAX_CACHED_PAGE_COUNT_PER_GROUP = 8;

    struct ThreadLocalPageCollection
    {
        /*
        m_alloc_timing 用于缓存线程局部的 GC 轮次，每次线程检查点时更新此值。
        */
        uint8_t m_alloc_timing;

        /*
        内存分配策略：
        ThreadLocalPageCollection 针对每一个分配组都缓存若干个 Page；如果某次分配中，页面报告其自身简单耗尽，
        则轮转到下一个页面；轮转时如果页面的 m_next_alloc_unit_head_offset 为空，则从 m_freed_unit_head_offset
        中提取；如果 m_freed_unit_head_offset 也为空，则称此时的分配组耗尽（即认为当前这些 Page 已经完全耗尽）。

        对于耗尽的分配组，ThreadLocalPageCollection 从 GlobalPageCollection 重新提取若干个空闲页面作为新的分配组
        旧的分配组将被继续缓存在 ThreadLocalPageCollection 中，
        */
        struct AllocFreeGroup
        {
            Page* m_allocating_page;
            size_t m_allocating_page_count;

            UnitHead* m_free_unit_head;
            size_t m_free_unit_count;
        };

        AllocFreeGroup m_current_allocating_page_for_group[TOTAL_GROUP_COUNT];

        // 优化：内联初始化单元属性，减少重复代码
        WOOMEM_FORCE_INLINE void init_allocated_unit(
            UnitHead* allocated_unit,
            woomem_GCUnitType unit_type) noexcept
        {
            // 使用位字段合并写入，减少内存访问次数
            // m_alloc_timing(4bit) + m_gc_type(4bit) 合并为一个字节
            allocated_unit->m_alloc_timing = m_alloc_timing & 0x0Fu;
            allocated_unit->m_gc_type = static_cast<uint8_t>(unit_type);
            allocated_unit->m_gc_age = NEW_BORN_GC_AGE;
            // 使用 relaxed 因为 m_allocated_status 会使用 release 保证可见性
            allocated_unit->m_gc_marked.store(WOOMEM_GC_MARKED_UNMARKED, std::memory_order_relaxed);
            // 最后设置 allocated_status，使用 release 确保之前的写入对其他线程可见
            allocated_unit->m_allocated_status.store(1, std::memory_order_release);
        }

        // 优化：将分配逻辑拆分，减少热路径的代码大小
        WOOMEM_FORCE_INLINE void* alloc(size_t unit_size, woomem_GCUnitType unit_type) noexcept
        {
            // 优化：直接计算查找索引，减少一次比较
            const size_t lookup_index = (unit_size + 7) >> 3;

            // 快速路径：小于等于 1024 字节
            if (WOOMEM_LIKELY(lookup_index < SMALL_UNIT_FAST_LOOKUP_TABLE_SIZE))
            {
                const PageGroupType alloc_group = SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[lookup_index];
                auto& current_alloc_group = m_current_allocating_page_for_group[alloc_group];

                // 1. Fast Path: Allocation from Free List (最热路径，优化为最少指令)
                if (WOOMEM_LIKELY(current_alloc_group.m_free_unit_count != 0))
                {
                    UnitHead* const allocated_unit = current_alloc_group.m_free_unit_head;

                    // 从用户数据区读取下一个空闲单元指针
                    current_alloc_group.m_free_unit_head =
                        *reinterpret_cast<UnitHead**>(allocated_unit + 1);
                    --current_alloc_group.m_free_unit_count;

                    // 初始化单元属性
                    init_allocated_unit(allocated_unit, unit_type);

                    return allocated_unit + 1;
                }

                // 2. Slow Path
                return alloc_slow_path(alloc_group, unit_type);
            }

            // 中等/大对象路径
            return alloc_medium_or_large(unit_size, unit_type);
        }

        // 中等和大对象的分配路径（冷路径）
        WOOMEM_NOINLINE void* alloc_medium_or_large(size_t unit_size, woomem_GCUnitType unit_type) noexcept
        {
            const auto alloc_group = get_page_group_type_for_size(unit_size);

            if (WOOMEM_LIKELY(alloc_group != PageGroupType::LARGE))
            {
                auto& current_alloc_group = m_current_allocating_page_for_group[alloc_group];

                // Fast Path for medium objects
                if (WOOMEM_LIKELY(current_alloc_group.m_free_unit_count != 0))
                {
                    UnitHead* const allocated_unit = current_alloc_group.m_free_unit_head;
                    current_alloc_group.m_free_unit_head =
                        *reinterpret_cast<UnitHead**>(allocated_unit + 1);
                    --current_alloc_group.m_free_unit_count;
                    init_allocated_unit(allocated_unit, unit_type);
                    return allocated_unit + 1;
                }
                return alloc_slow_path(alloc_group, unit_type);
            }
            else
            {
                // TODO: Large alloc
                abort();
            }
        }

        // 慢速路径：从页面分配
        WOOMEM_NOINLINE void* alloc_slow_path(PageGroupType alloc_group, woomem_GCUnitType unit_type) noexcept
        {
            auto& current_alloc_group = m_current_allocating_page_for_group[alloc_group];
            if (WOOMEM_UNLIKELY(0 == current_alloc_group.m_allocating_page_count))
            {
                if (WOOMEM_UNLIKELY(!lazy_init_group(alloc_group)))
                {
                    return nullptr;
                }
            }

            auto*& current_alloc_page = current_alloc_group.m_allocating_page;
            UnitHead* allocated_unit;

            while (true)
            {
                allocated_unit = current_alloc_page->try_allocate_unit_from_page();
                if (WOOMEM_LIKELY(allocated_unit != nullptr))
                {
                    break;
                }

                // Page run out, try next page in this group.
                auto* const next_page = current_alloc_page->m_page_head.m_next_page;

                if (WOOMEM_LIKELY(next_page->try_tidy_page()))
                {
                    current_alloc_page = next_page;
                    continue;
                }

                // This page run out, drop it and try get new pages from global pool.
                auto* const new_page = g_global_page_collection.try_get_free_page(alloc_group);

                if (WOOMEM_LIKELY(new_page != nullptr))
                {
                    // Got a new page from global pool.
                    current_alloc_page->m_page_head.m_next_page = new_page;

                    if (current_alloc_group.m_allocating_page_count <
                        THREAD_LOCAL_POOL_MAX_CACHED_PAGE_COUNT_PER_GROUP)
                    {
                        // Cache this page in local pool.
                        ++current_alloc_group.m_allocating_page_count;

                        // Link new page to next_page.
                        new_page->m_page_head.m_next_page = next_page;
                    }
                    else
                    {
                        // Drop run out page.
                        new_page->m_page_head.m_next_page = next_page->m_page_head.m_next_page;
                        next_page->m_page_head.m_abondon_page_flag.store(1, std::memory_order_release);
                    }
                    current_alloc_page = new_page;
                    continue;
                }

                // We have tried all method to get new unit, alloc failed...
                return nullptr;
            }

            // 初始化分配的单元（复用内联函数）
            init_allocated_unit(allocated_unit, unit_type);

            return allocated_unit + 1;
        }

        WOOMEM_FORCE_INLINE void free(void* unit) noexcept
        {
            UnitHead* const freeing_unit_head =
                reinterpret_cast<UnitHead*>(
                    reinterpret_cast<char*>(unit) - sizeof(UnitHead));

            // 获取 page group 类型
            const PageGroupType group_type =
                freeing_unit_head->m_parent_page->m_page_head.m_page_belong_to_group;
            auto& group = m_current_allocating_page_for_group[group_type];

            // 优化：使用快速释放路径（避免 atomic exchange，假设同一线程分配和释放）
            if (WOOMEM_LIKELY(freeing_unit_head->try_free_this_unit_head()))
            {
                // 将释放的单元加入本地空闲列表
                *reinterpret_cast<UnitHead**>(unit) = group.m_free_unit_head;
                group.m_free_unit_head = freeing_unit_head;
                ++group.m_free_unit_count;
            }
        }

        WOOMEM_NOINLINE bool lazy_init_group(PageGroupType group_type) noexcept
        {
            auto& group = m_current_allocating_page_for_group[group_type];

            // 只分配一个页面作为初始页面，后续按需增加
            Page* initial_page = g_global_page_collection.try_get_free_page(group_type);
            if (WOOMEM_UNLIKELY(initial_page == nullptr))
            {
                return false;
            }

            // 形成单元素循环链表
            initial_page->m_page_head.m_next_page = initial_page;

            group.m_allocating_page = initial_page;
            group.m_allocating_page_count = 1;

            return true;
        }

        ThreadLocalPageCollection() noexcept
            : m_alloc_timing{ 0 }
        {
            for (PageGroupType t = PageGroupType::SMALL_8;
                t < PageGroupType::LARGE;
                t = static_cast<PageGroupType>(static_cast<uint8_t>(t + 1)))
            {
                auto& group = m_current_allocating_page_for_group[t];
                group.m_allocating_page = nullptr;
                group.m_allocating_page_count = 0;
                group.m_free_unit_head = nullptr;
                group.m_free_unit_count = 0;
            }
        }
        ~ThreadLocalPageCollection()
        {
            if (Chunk::g_current_chunk.load(std::memory_order_acquire) == nullptr)
            {
                // Already shutdown.
                return;
            }

            for (PageGroupType t = PageGroupType::SMALL_8;
                t < PageGroupType::LARGE;
                t = static_cast<PageGroupType>(static_cast<uint8_t>(t + 1)))
            {
                auto& group = m_current_allocating_page_for_group[t];

                // Drop all cached free units.
                UnitHead* freeing_unit_head = group.m_free_unit_head;
                while (freeing_unit_head != nullptr)
                {
                    UnitHead* const next_freeing_unit_head =
                        *reinterpret_cast<UnitHead**>(
                            reinterpret_cast<char*>(freeing_unit_head) + sizeof(UnitHead));

                    freeing_unit_head->m_parent_page->drop_back_unit_in_this_page_asyncly(freeing_unit_head);
                    freeing_unit_head = next_freeing_unit_head;
                }

                if (group.m_allocating_page_count > 0)
                {
                    Page* const first_freeing_page = group.m_allocating_page;
                    Page* freeing_page = first_freeing_page;
                    do
                    {
                        Page* const next_page = freeing_page->m_page_head.m_next_page;
                        g_global_page_collection.return_free_page(freeing_page);
                        freeing_page = next_page;

                    } while (freeing_page != first_freeing_page);
                }
            }
        }

        ThreadLocalPageCollection(const ThreadLocalPageCollection&) = delete;
        ThreadLocalPageCollection(ThreadLocalPageCollection&&) = delete;
        ThreadLocalPageCollection& operator=(const ThreadLocalPageCollection&) = delete;
        ThreadLocalPageCollection& operator=(ThreadLocalPageCollection&&) = delete;
    };
    thread_local ThreadLocalPageCollection t_tls_page_collection;

    atomic<Chunk*> Chunk::g_current_chunk;
}

using namespace woomem_cppimpl;

void woomem_init(void)
{
    assert(Chunk::g_current_chunk.load(std::memory_order_acquire) == nullptr);

    Chunk::g_current_chunk.store(
        Chunk::create_new_chunk(), std::memory_order_release);
}
void woomem_shutdown(void)
{
    Chunk* current_chunk =
        Chunk::g_current_chunk.load(std::memory_order_acquire);

    assert(current_chunk != nullptr);

    do
    {
        Chunk* last_chunk = current_chunk->m_last_chunk;

        current_chunk->~Chunk();
        free(current_chunk);

        current_chunk = last_chunk;

    } while (current_chunk != nullptr);

    Chunk::g_current_chunk.store(
        nullptr,
        std::memory_order_release);
}

/* OPTIONAL */ void* woomem_alloc_normal(size_t size)
{
    return t_tls_page_collection.alloc(size, WOOMEM_GC_UNIT_TYPE_NORMAL);
}
/* OPTIONAL */ void* woomem_alloc_auto_mark(size_t size)
{
    return t_tls_page_collection.alloc(size, WOOMEM_GC_UNIT_TYPE_AUTO_MARK);
}
/* OPTIONAL */ void* woomem_alloc_gcunit(size_t size)
{
    return t_tls_page_collection.alloc(size, WOOMEM_GC_UNIT_TYPE_IS_GCUNIT);
}

void woomem_free(void* ptr)
{
    t_tls_page_collection.free(ptr);
}