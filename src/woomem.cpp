#include "woomem.h"
#include "woomem_os_mmap.h"

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <new>
#include <vector>
#include <algorithm>
#include <map>

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
    class RWSpin
    {
        atomic_uint32_t m_spin_mark;
        static constexpr uint32_t WRITE_LOCK_MASK = 0x80000000u;

    public:
        RWSpin() noexcept : m_spin_mark{ 0 } {}
        RWSpin(const RWSpin&) = delete;
        RWSpin(RWSpin&&) = delete;
        RWSpin& operator=(const RWSpin&) = delete;
        RWSpin& operator=(RWSpin&&) = delete;

        void lock() noexcept
        {
            do
            {
                uint32_t prev_status =
                    m_spin_mark.fetch_or(
                        WRITE_LOCK_MASK,
                        std::memory_order_acq_rel);

                if (WOOMEM_LIKELY(0 == (prev_status & WRITE_LOCK_MASK)))
                {
                    // Lock acquired, check until all readers are gone.
                    if (prev_status != 0)
                    {
                        // Still have other readers.
                        do
                        {
                            WOOMEM_PAUSE();
                            prev_status = m_spin_mark.load(std::memory_order_acquire);

                        } while (prev_status != WRITE_LOCK_MASK);
                    }
                    break;
                }

                // Or, someone else is holding the write lock, wait.
                WOOMEM_PAUSE();
            } while (true);
        }
        void unlock() noexcept
        {
            assert(m_spin_mark.load(std::memory_order_relaxed) == WRITE_LOCK_MASK);
            m_spin_mark.store(0, std::memory_order_release);
        }
        void lock_shared() noexcept
        {
            do
            {
                uint32_t prev_status = m_spin_mark.load(std::memory_order_acquire);
                if (WOOMEM_LIKELY(0 == (prev_status & WRITE_LOCK_MASK)))
                {
                    // No write lock mask, try to add reader count.
                    do
                    {
                        if (m_spin_mark.compare_exchange_strong(
                            prev_status,
                            prev_status + 1,
                            std::memory_order_release,
                            std::memory_order_relaxed))
                            // Ok, acquired shared lock.
                            return;

                        if (0 != (prev_status & WRITE_LOCK_MASK))
                            // Write lock mask appeared, roll back.
                            break;

                        WOOMEM_PAUSE();

                    } while (true);
                }
                WOOMEM_PAUSE();
            } while (true);
        }
        void unlock_shared() noexcept
        {
            const uint32_t prev_status =
                m_spin_mark.fetch_sub(1, std::memory_order_release);

            (void)prev_status;
            assert(0 != (prev_status & ~WRITE_LOCK_MASK));
        }
    };

    struct GlobalMarkContext
    {
        woomem_UserContext          m_user_ctx;
        woomem_MarkCallbackFunc     m_marker;
        woomem_DestroyCallbackFunc  m_destroyer;
    };
    GlobalMarkContext g_global_mark_ctx;

    constexpr size_t PAGE_SIZE = 64 * 1024;             // 64KB

    /* A Chunk will store many page, each page will be used for One specified allocated size */
    constexpr size_t CHUNK_SIZE = 128 * 1024 * 1024;    // 128MB

    constexpr size_t CARDTABLE_SIZE_PER_BIT = 512;      // 512 Bytes per bit

    constexpr size_t BASE_ALIGNMENT = 8;                // 8 Bytes

    constexpr size_t LARGE_SPACE_HEAD_SIZE = 32;        // 32 Bytes header for large space

    constexpr size_t MOST_LARGE_UNIT_SIZE = 16 * PAGE_SIZE - LARGE_SPACE_HEAD_SIZE;

    constexpr uint8_t NEW_BORN_GC_AGE = 15;

    constexpr size_t THREAD_LOCAL_POOL_MAX_CACHED_PAGE_COUNT_PER_GROUP = 8;

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

        FAST_AND_MIDIUM_GROUP_COUNT,

        LARGE_PAGES_1 = FAST_AND_MIDIUM_GROUP_COUNT,
        LARGE_PAGES_2,
        LARGE_PAGES_3,
        LARGE_PAGES_4,
        LARGE_PAGES_5,
        LARGE_PAGES_6,
        LARGE_PAGES_7,
        LARGE_PAGES_8,
        LARGE_PAGES_9,
        LARGE_PAGES_10,
        LARGE_PAGES_11,
        LARGE_PAGES_12,
        LARGE_PAGES_13,
        LARGE_PAGES_14,
        LARGE_PAGES_15,
        LARGE_PAGES_16,

        TOTAL_GROUP_COUNT,

        HUGE = TOTAL_GROUP_COUNT,
    };
    constexpr size_t PAGE_GROUP_NEED_PAGE_COUNTS[] =
    {
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,

        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,

        // LARGE_PAGES
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
    };
    static_assert(
        PAGE_GROUP_NEED_PAGE_COUNTS[PageGroupType::LARGE_PAGES_1] == 1
        && PAGE_GROUP_NEED_PAGE_COUNTS[PageGroupType::LARGE_PAGES_2] == 2
        && PAGE_GROUP_NEED_PAGE_COUNTS[PageGroupType::LARGE_PAGES_16] == 16,
        "PAGE_GROUP_NEED_PAGE_COUNTS must be correct.");

    constexpr size_t UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[] =
    {
        // Small page groups
        8, 24, 40, 56, 88, 128, 192, 264, 344, 488, 704, 920, 1024,

        // Medium page groups
        1440, 2168, 3104, 4352, 6536, 9344, 13088, 21824,

        // Large page groups
        65504, 131040, 196576, 262112, 327648, 393184, 458720, 524256,
        589792, 655328, 720864, 786400, 851936, 917472, 983008, 1048544,
    };
    static_assert(UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[PageGroupType::SMALL_1024] == 1024,
        "UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP must be correct.");
    static_assert(UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[PageGroupType::MIDIUM_21824] == 21824,
        "UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP must be correct.");

    constexpr size_t MAX_SMALL_UNIT_SIZE = 1024;
    constexpr size_t SMALL_UNIT_FAST_LOOKUP_TABLE_SIZE =
        (MAX_SMALL_UNIT_SIZE + 7) / 8 + 1;

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
    WOOMEM_FORCE_INLINE PageGroupType get_page_group_type_for_size(size_t size) noexcept
    {
        // 快速路径：小于等于 1024 字节使用查找表
        if (WOOMEM_LIKELY(size < MAX_SMALL_UNIT_SIZE))
            return SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[(size + 7) >> 3];
        if (size <= 1440)
            return MIDIUM_1440;
        if (size <= 2168)
            return MIDIUM_2168;
        if (size <= 3104)
            return MIDIUM_3104;
        if (size <= 4352)
            return MIDIUM_4352;
        if (size <= 6536)
            return MIDIUM_6536;
        if (size <= 9344)
            return MIDIUM_9344;
        if (size <= 13088)
            return MIDIUM_13088;
        if (size <= 21824)
            return MIDIUM_21824;

        if (size <= MOST_LARGE_UNIT_SIZE)
        {
            return static_cast<PageGroupType>(
                LARGE_PAGES_1 + ((size + LARGE_SPACE_HEAD_SIZE - 1) >> 5 /* div PAGE_SIZE */));
        }
        return HUGE;
    }

    union Page;
    struct LargePageUnitHead;

    struct PageHead
    {
        union
        {
            Page* m_next_page;
            LargePageUnitHead* m_next_large_unit;
        };

        // Used for finding chunk head.
        uint16_t m_page_index_in_chunk;

        static_assert(CHUNK_SIZE / PAGE_SIZE < UINT16_MAX,
            "Cannot store page index in chunk.");

        PageGroupType m_page_belong_to_group;

        // 如果页面彻底耗尽，没有空余单元，此页面将被抛弃，TLS Pool 将尝试重新拉取一个新的 Page
        // m_abondon_page_flag 将在页面被抛弃时设置
        atomic_uint8_t  m_abondon_page_flag;

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

        uint8_t         m_alloc_timing;
        uint8_t /* woomem_GCUnitType */ m_gc_type;
        uint8_t         m_gc_age;
        atomic_uint8_t  m_gc_marked;

        uint16_t m_next_alloc_unit_offset;
        WOOMEM_FORCE_INLINE void destroy() noexcept
        {
            if (m_gc_type & WOOMEM_GC_UNIT_TYPE_HAS_FINALIZER)
            {
                g_global_mark_ctx.m_destroyer(
                    reinterpret_cast<void*>(this + 1),
                    g_global_mark_ctx.m_user_ctx);
            }
        }
        WOOMEM_FORCE_INLINE bool try_free_this_unit_head() noexcept
        {
            if (0 != m_allocated_status.exchange(0, std::memory_order_relaxed))
            {
                destroy();
                return true;
            }
            return false;
        }
        WOOMEM_FORCE_INLINE void fast_free_unit_manually() noexcept
        {
            assert(1 == m_allocated_status.load(std::memory_order_relaxed));
            m_allocated_status.store(0, std::memory_order_relaxed);
            destroy();
        }
        WOOMEM_FORCE_INLINE void init_unit_head() noexcept
        {
            m_allocated_status.store(0, std::memory_order_relaxed);

            /* unit_head->m_alloc_timing will be set when allocate. */
            m_gc_age = NEW_BORN_GC_AGE;
            m_gc_marked.store(
                WOOMEM_GC_MARKED_UNMARKED, std::memory_order_relaxed);
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
            assert(group_type < PageGroupType::LARGE_PAGES_1);

            m_page_head.m_next_page = nullptr;
            m_page_head.m_page_belong_to_group = group_type;
            m_page_head.m_abondon_page_flag.store(0, std::memory_order_relaxed);
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
                unit_head->init_unit_head();

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

            const uint16_t free_list = m_page_head.m_freed_unit_head_offset.exchange(
                0, std::memory_order_acq_rel);

            if (free_list != 0)
            {
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
            UnitHead* const unit_head =
                reinterpret_cast<UnitHead*>(valid_page_unit) - 1;

            unit_head->m_parent_page->free_unit_in_this_page_asyncly(unit_head);
        }
    };
    static_assert(sizeof(Page) == PAGE_SIZE,
        "Page size must be equal to PAGE_SIZE");

    struct LargePageUnitHead
    {
        PageHead m_page_head;
        UnitHead m_unit_head;

        void init_for_large_page_unit(PageGroupType group_type)
        {
            m_page_head.m_next_large_unit = nullptr;
            m_page_head.m_page_belong_to_group = group_type;
            m_page_head.m_abondon_page_flag.store(0, std::memory_order_relaxed);

            m_page_head.m_freed_unit_head_offset.store(0, std::memory_order_relaxed);
            m_page_head.m_next_alloc_unit_head_offset = 0;

            m_unit_head.m_parent_page = nullptr;
            m_unit_head.init_unit_head();
        }
    };

    struct Chunk
    {
        Chunk* m_last_chunk;

        std::atomic_size_t m_next_commiting_page_count;
        std::atomic_size_t m_commited_page_count;

        Page* const m_reserved_address_begin;
        Page* const m_reserved_address_end;

        // std::atomic_uint64_t m_cardtable[CHUNK_SIZE / CARDTABLE_SIZE_PER_BIT / 64];
        // uint8_t m_multi_page_unit_flags[CHUNK_SIZE / PAGE_SIZE];

        static_assert(std::atomic_uint64_t::is_always_lock_free,
            "atomic_uint64_t must be lock free for performance");

        Chunk(void* reserved_address) noexcept
            : m_last_chunk(nullptr)
            , m_next_commiting_page_count{ 0 }
            , m_commited_page_count{ 0 }
            , m_reserved_address_begin(reinterpret_cast<Page*>(reserved_address))
            , m_reserved_address_end(reinterpret_cast<Page*>(
                reinterpret_cast<uint8_t*>(reserved_address) + CHUNK_SIZE))
            // , m_cardtable{}
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

        /* OPTIONAL */ void* allocate_new_page_in_chunk(
            PageGroupType page_group, bool* out_page_run_out) noexcept
        {
            assert(page_group < TOTAL_GROUP_COUNT);

            const size_t need_group_count = PAGE_GROUP_NEED_PAGE_COUNTS[page_group];

            size_t new_page_index =
                m_next_commiting_page_count.load(std::memory_order_relaxed);

            do
            {
                const size_t desired_page_index = new_page_index + need_group_count;
                if (WOOMEM_UNLIKELY(desired_page_index > CHUNK_SIZE / PAGE_SIZE))
                {
                    // No more page in this chunk.
                    *out_page_run_out = true;
                    return nullptr;
                }

                if (m_next_commiting_page_count.compare_exchange_weak(
                    new_page_index,
                    desired_page_index,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed))
                {
                    // Page space reserved successfully.
                    *out_page_run_out = false;
                    break;
                }

            } while (true);

            void* new_alloc_page = &m_reserved_address_begin[new_page_index];
            const auto status = woomem_os_commit_memory(
                new_alloc_page,
                need_group_count * PAGE_SIZE);

            if (WOOMEM_LIKELY(status == 0))
            {
                // Init this page.
                // NOTE: Even we commit multiple pages, only the first page need init.
                if (page_group < PageGroupType::LARGE_PAGES_1)
                {
                    Page* const now_alloc_page = reinterpret_cast<Page*>(new_alloc_page);

                    now_alloc_page->m_page_head.m_page_index_in_chunk =
                        static_cast<uint16_t>(now_alloc_page - m_reserved_address_begin);
                    now_alloc_page->reinit_page_with_group(
                        page_group);

                }
                else
                {
                    LargePageUnitHead* const now_alloc_large_unit =
                        reinterpret_cast<LargePageUnitHead*>(new_alloc_page);

                    now_alloc_large_unit->m_page_head.m_page_index_in_chunk =
                        static_cast<uint16_t>(
                            reinterpret_cast<Page*>(new_alloc_page) - m_reserved_address_begin);
                    now_alloc_large_unit->init_for_large_page_unit(page_group);
                }

                // Wait until other threads finish committing.
                do
                {
                    size_t expected_commited = new_page_index;
                    if (m_commited_page_count.compare_exchange_weak(
                        expected_commited,
                        new_page_index + need_group_count,
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

            // Never reach here.
            abort();
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

            return new (chunk_storage) Chunk(reserved_address);
        }
    };

    struct HugeUnitHead
    {
        /*
        LAYOUT:
            |               Head                | Body   |  CardTable |
            | HugeUnitHead   LargePageUnitHead |        |             |
        */

        size_t m_fact_unit_size;
        // std::atomic_uint64_t* m_cardtable;
    };

    // TODO: Need impl, and consider if m_chunk_map cannot alloc memory,
    //       how to handle that case.
    class AddrToBlockFastLookupTable
    {
        RWSpin m_rwspin;
        map<void*, /* OPTIONAL */ Chunk*> m_chunk_map;
    public:
        WOOMEM_FORCE_INLINE void add_new_chunk(Chunk* new_chunk_instance) noexcept
        {
            m_rwspin.lock();
            {
                auto result = m_chunk_map.insert(
                    make_pair(
                        new_chunk_instance->m_reserved_address_begin,
                        new_chunk_instance));

                (void)result;
                assert(result.second);
            }
            m_rwspin.unlock();
        }
        // void add_huge_unit(..) noexcept;
    };

    struct GlobalPageCollection
    {
        atomic<Chunk*> m_current_chunk;
        AddrToBlockFastLookupTable m_addr_to_chunk_table;

        atomic<Page*> m_free_group_page_list[FAST_AND_MIDIUM_GROUP_COUNT];

        // 用于储存可分配的大对象和巨型对象实例，注意，TOTAL_GROUP_COUNT 的前 
        // FAST_AND_MIDIUM_GROUP_COUNT 项并不使用，仅作占位（避免每次都减去这些值）。
        atomic<LargePageUnitHead*> m_free_large_unit_list[TOTAL_GROUP_COUNT];

        // HUGE 对象并不使用 Page 进行管理，释放操作也应当立即发生。
        // TODO: 需要考虑如何高效地，在有 HUGE 对象的情况下，能够快速校验地址是否合法。

        /* OPTIONAL */ void* allocate_new_page(PageGroupType page_group)
        {
            Chunk* current_chunk =
                m_current_chunk.load(std::memory_order_relaxed);

            do
            {
                bool page_run_out;
                void* new_page = current_chunk->allocate_new_page_in_chunk(
                    page_group, &page_run_out);

                if (WOOMEM_UNLIKELY(page_run_out))
                {
                    // Check last chunk?
                    current_chunk = current_chunk->m_last_chunk;

                    if (WOOMEM_UNLIKELY(current_chunk == nullptr))
                    {
                        current_chunk = Chunk::create_new_chunk();
                        m_addr_to_chunk_table.add_new_chunk(current_chunk);

                        if (WOOMEM_UNLIKELY(current_chunk == nullptr))
                            // Failed to alloc chunk..
                            return nullptr;

                        while (!m_current_chunk.compare_exchange_weak(
                            current_chunk->m_last_chunk,
                            current_chunk,
                            std::memory_order_release,
                            std::memory_order_acquire))
                        {
                            WOOMEM_PAUSE();
                        }
                    }
                    continue;
                }
                // new_page might be nullptr if commit memory failed.
                return new_page;

            } while (true);

            // Never reach here.
            abort();
        }

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
            return reinterpret_cast<Page*>(allocate_new_page(group_type));
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

        void free_large_unit_asyncly(void* large_unit) noexcept
        {
            LargePageUnitHead* large_unit_head =
                reinterpret_cast<LargePageUnitHead*>(large_unit) - 1;

            auto& large_unit_list = m_free_large_unit_list[
                large_unit_head->m_page_head.m_page_belong_to_group];

            large_unit_head->m_page_head.m_next_large_unit =
                large_unit_list.load(std::memory_order_relaxed);
            while (!large_unit_list.compare_exchange_weak(
                large_unit_head->m_page_head.m_next_large_unit,
                large_unit_head,
                std::memory_order_release,
                std::memory_order_relaxed))
            {
                WOOMEM_PAUSE();
            }
        }
        /* OPTIONAL */ void* try_alloc_large_unit(PageGroupType group_type) noexcept
        {
            auto& large_unit_list = m_free_large_unit_list[group_type];
            LargePageUnitHead* free_large_unit =
                large_unit_list.load(std::memory_order_acquire);
            while (free_large_unit != nullptr)
            {
                if (large_unit_list.compare_exchange_weak(
                    free_large_unit,
                    free_large_unit->m_page_head.m_next_large_unit,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                {
                    // Successfully got the large unit
                    free_large_unit->m_page_head.m_next_large_unit = nullptr;
                    return free_large_unit + 1;
                }
                // CAS failed, free_large_unit is updated to current value, retry
                WOOMEM_PAUSE();
            }
            // No free large unit in pool, allocate a new one from Chunk
            void* new_large_unit = allocate_new_page(group_type);
            if (WOOMEM_LIKELY(new_large_unit != nullptr))
            {
                LargePageUnitHead* large_unit_head =
                    reinterpret_cast<LargePageUnitHead*>(new_large_unit);

                return large_unit_head + 1;
            }
            return nullptr;
        }
    };
    GlobalPageCollection g_global_page_collection{};

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

        AllocFreeGroup m_current_allocating_page_for_group[FAST_AND_MIDIUM_GROUP_COUNT];

        // 优化：内联初始化单元属性，减少重复代码
        WOOMEM_FORCE_INLINE void init_allocated_unit(
            UnitHead* allocated_unit,
            uint8_t /* 4bits */ unit_type_mask) noexcept
        {
            // 使用位字段合并写入，减少内存访问次数
            // m_alloc_timing(4bit) + m_gc_type(4bit) 合并为一个字节
            allocated_unit->m_alloc_timing = m_alloc_timing & 0x0Fu;
            allocated_unit->m_gc_type = unit_type_mask;
            allocated_unit->m_gc_age = NEW_BORN_GC_AGE;
            // 使用 relaxed 因为 m_allocated_status 会使用 release 保证可见性
            allocated_unit->m_gc_marked.store(WOOMEM_GC_MARKED_UNMARKED, std::memory_order_relaxed);
            // 最后设置 allocated_status，使用 release 确保之前的写入对其他线程可见
            allocated_unit->m_allocated_status.store(1, std::memory_order_release);
        }

        // 优化：将分配逻辑拆分，减少热路径的代码大小
        WOOMEM_FORCE_INLINE void* alloc(size_t unit_size, uint8_t /* 4bits */ unit_type_mask) noexcept
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
                    init_allocated_unit(allocated_unit, unit_type_mask);

                    return allocated_unit + 1;
                }

                // 2. Slow Path
                return alloc_slow_path(alloc_group, unit_type_mask);
            }

            // 中等/大对象路径
            return alloc_medium_or_large(unit_size, unit_type_mask);
        }

        // 中等和大对象的分配路径（冷路径）
        WOOMEM_NOINLINE void* alloc_medium_or_large(size_t unit_size, uint8_t /* 4bits */ unit_type_mask) noexcept
        {
            const auto alloc_group = get_page_group_type_for_size(unit_size);

            if (WOOMEM_LIKELY(alloc_group < PageGroupType::LARGE_PAGES_1))
            {
                auto& current_alloc_group = m_current_allocating_page_for_group[alloc_group];

                // Fast Path for medium objects
                if (WOOMEM_LIKELY(current_alloc_group.m_free_unit_count != 0))
                {
                    UnitHead* const allocated_unit = current_alloc_group.m_free_unit_head;
                    current_alloc_group.m_free_unit_head =
                        *reinterpret_cast<UnitHead**>(allocated_unit + 1);
                    --current_alloc_group.m_free_unit_count;
                    init_allocated_unit(allocated_unit, unit_type_mask);
                    return allocated_unit + 1;
                }
                return alloc_slow_path(alloc_group, unit_type_mask);
            }
            else if (alloc_group != PageGroupType::HUGE)
            {
                assert(alloc_group < PageGroupType::HUGE);

                void* const allocated_unit =
                    g_global_page_collection.try_alloc_large_unit(alloc_group);

                if (WOOMEM_LIKELY(allocated_unit != nullptr))
                {
                    UnitHead* const allocated_unit_head =
                        reinterpret_cast<UnitHead*>(allocated_unit) - 1;

                    assert(allocated_unit_head->m_parent_page == nullptr);
                    assert(0 == allocated_unit_head->m_allocated_status.load(
                        std::memory_order_relaxed));

                    if (WOOMEM_LIKELY(allocated_unit != nullptr))
                        init_allocated_unit(allocated_unit_head, unit_type_mask);
                }
                return allocated_unit;
            }
            else
            {
                // TODO: Huge alloc
                abort();
            }
        }

        // 慢速路径：从页面分配
        WOOMEM_NOINLINE void* alloc_slow_path(PageGroupType alloc_group, uint8_t /* 4bits */ unit_type_mask) noexcept
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
            init_allocated_unit(allocated_unit, unit_type_mask);

            return allocated_unit + 1;
        }

        WOOMEM_FORCE_INLINE void free(void* unit) noexcept
        {
            UnitHead* const freeing_unit_head =
                reinterpret_cast<UnitHead*>(unit) - 1;

            freeing_unit_head->fast_free_unit_manually();

            if (WOOMEM_LIKELY(freeing_unit_head->m_parent_page != nullptr))
            {
                // 获取 page group 类型
                const PageGroupType group_type =
                    freeing_unit_head->m_parent_page->m_page_head.m_page_belong_to_group;
                auto& group = m_current_allocating_page_for_group[group_type];

                // 将释放的单元加入本地空闲列表
                *reinterpret_cast<UnitHead**>(unit) = group.m_free_unit_head;
                group.m_free_unit_head = freeing_unit_head;
                ++group.m_free_unit_count;
            }
            else
            {
                LargePageUnitHead* const large_unit_head =
                    reinterpret_cast<LargePageUnitHead*>(unit) - 1;

                if (WOOMEM_LIKELY(large_unit_head->m_page_head.m_page_belong_to_group != PageGroupType::HUGE))
                {
                    // 大对象，返回到全局大对象池
                    g_global_page_collection.free_large_unit_asyncly(large_unit_head + 1);
                }
                else
                {
                    // HUGE 对象，直接释放内存
                    // TODO;
                    abort();
                }
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
                t < PageGroupType::FAST_AND_MIDIUM_GROUP_COUNT;
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
            if (g_global_page_collection.m_current_chunk.load(std::memory_order_acquire) == nullptr)
            {
                // Already shutdown.
                return;
            }

            for (PageGroupType t = PageGroupType::SMALL_8;
                t < PageGroupType::FAST_AND_MIDIUM_GROUP_COUNT;
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
}

using namespace woomem_cppimpl;

void woomem_init(
    woomem_UserContext user_ctx,
    woomem_MarkCallbackFunc marker,
    woomem_DestroyCallbackFunc destroyer)
{
    assert(g_global_page_collection.m_current_chunk.load(
        std::memory_order_acquire) == nullptr);

    g_global_mark_ctx.m_user_ctx = user_ctx;
    g_global_mark_ctx.m_marker = marker;
    g_global_mark_ctx.m_destroyer = destroyer;

    g_global_page_collection.m_current_chunk.store(
        Chunk::create_new_chunk(), std::memory_order_release);
}
void woomem_shutdown(void)
{
    Chunk* current_chunk =
        g_global_page_collection.m_current_chunk.load(std::memory_order_acquire);

    assert(current_chunk != nullptr);

    do
    {
        Chunk* last_chunk = current_chunk->m_last_chunk;

        current_chunk->~Chunk();
        free(current_chunk);

        current_chunk = last_chunk;

    } while (current_chunk != nullptr);

    g_global_page_collection.m_current_chunk.store(
        nullptr,
        std::memory_order_release);
}

/* OPTIONAL */ void* woomem_alloc_normal(size_t size)
{
    return t_tls_page_collection.alloc(size, 0);
}
void* woomem_alloc_attrib(size_t size, woomem_GCUnitTypeMask attrib)
{
    return t_tls_page_collection.alloc(size, static_cast<uint8_t>(attrib));
}

/* OPTIONAL */ void* woomem_realloc(void* ptr, size_t new_size)
{
    assert(ptr != nullptr);

    UnitHead* const old_unit_head = reinterpret_cast<UnitHead*>(ptr) - 1;
    Page* const old_page = old_unit_head->m_parent_page;
    const PageGroupType old_group_type = old_page->m_page_head.m_page_belong_to_group;

    // 获取新大小对应的分配组
    const PageGroupType new_group_type = get_page_group_type_for_size(new_size);

    // 如果新大小比当前分配组小，检查是否值得缩小
    // 只有当新分配组比旧分配组小至少2级时才进行缩小，避免频繁的收缩/扩展
    if (new_group_type <= old_group_type)
    {
        // 计算分配组差距
        const uint8_t group_diff =
            static_cast<uint8_t>(old_group_type) - static_cast<uint8_t>(new_group_type);

        // 如果差距小于2级，保持原分配不变（避免内存抖动）
        if (group_diff < 2)
        {
            return ptr;
        }
        // 差距较大时，进行缩小以节省内存
    }

    // 需要分配新内存（扩大或显著缩小的情况）
    // 保留原有的 GC 类型
    void* new_ptr = woomem_alloc_attrib(
        new_size,
        static_cast<woomem_GCUnitTypeMask>(
            old_unit_head->m_gc_type));

    if (WOOMEM_UNLIKELY(new_ptr == nullptr))
    {
        // 分配失败，保持原内存不变
        return nullptr;
    }

    // 获取旧的实际分配大小
    const size_t old_unit_size = (old_group_type < PageGroupType::HUGE)
        ? UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[old_group_type]
        : 0; // HUGE 类型暂不支持

    // 复制数据：复制 min(old_unit_size, new_size) 字节
    memcpy(new_ptr, ptr, (old_unit_size < new_size) ? old_unit_size : new_size);

    // 继承 GC 相关属性（可选：根据需求决定是否继承）
    UnitHead* const new_unit_head = reinterpret_cast<UnitHead*>(new_ptr) - 1;
    new_unit_head->m_gc_age = old_unit_head->m_gc_age;
    new_unit_head->m_gc_marked.store(
        old_unit_head->m_gc_marked.load(std::memory_order_relaxed),
        std::memory_order_relaxed);

    // 释放旧内存
    woomem_free(ptr);

    return new_ptr;
}

void woomem_free(void* ptr)
{
    t_tls_page_collection.free(ptr);
}
