#include "woomem.h"
#include "woomem_os_mmap.h"

#include <cstdio>
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
#include <unordered_set>
#include <forward_list>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

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
                        memory_order::memory_order_acq_rel);

                if (WOOMEM_LIKELY(0 == (prev_status & WRITE_LOCK_MASK)))
                {
                    // Lock acquired, check until all readers are gone.
                    if (prev_status != 0)
                    {
                        // Still have other readers.
                        do
                        {
                            WOOMEM_PAUSE();
                            prev_status = m_spin_mark.load(memory_order::memory_order_acquire);

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
            assert(m_spin_mark.load(memory_order::memory_order_relaxed) == WRITE_LOCK_MASK);
            m_spin_mark.store(0, memory_order::memory_order_release);
        }
        void lock_shared() noexcept
        {
            do
            {
                uint32_t prev_status = m_spin_mark.load(memory_order::memory_order_acquire);
                if (WOOMEM_LIKELY(0 == (prev_status & WRITE_LOCK_MASK)))
                {
                    // No write lock mask, try to add reader count.
                    do
                    {
                        if (m_spin_mark.compare_exchange_strong(
                            prev_status,
                            prev_status + 1,
                            memory_order::memory_order_release,
                            memory_order::memory_order_relaxed))
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
                m_spin_mark.fetch_sub(1, memory_order::memory_order_release);

            (void)prev_status;
            assert(0 != (prev_status & ~WRITE_LOCK_MASK));
        }
    };
    class Spin
    {
        std::atomic_flag m_spin = ATOMIC_FLAG_INIT;
    public:
        Spin() = default;

        Spin(const Spin&) = delete;
        Spin(Spin&&) = delete;
        Spin& operator=(const Spin&) = delete;
        Spin& operator=(Spin&&) = delete;

        void lock() noexcept
        {
            while (m_spin.test_and_set(memory_order::memory_order_acquire))
                WOOMEM_PAUSE();
        }
        void unlock()
        {
            m_spin.clear(memory_order::memory_order_release);
        }
    };

    namespace gc
    {
        struct GlobalGCMethods
        {
            woomem_UserContext          m_user_ctx;

            woomem_MarkCallbackFunc     m_marker;
            woomem_DestroyCallbackFunc  m_destroyer;
            woomem_RootMarkingFunc      m_root_marking;
        };
        GlobalGCMethods g_global_gc_methods;
    }

    constexpr size_t PAGE_SIZE = 64 * 1024;             // 64KB

    /* A Chunk will store many page, each page will be used for One specified allocated size */
    constexpr size_t CHUNK_SIZE = 128 * 1024 * 1024;    // 128MB

    constexpr size_t PAGE_COUNT_PER_CHUNK = CHUNK_SIZE / PAGE_SIZE;

    constexpr size_t CARDTABLE_SIZE_PER_BIT = 512;      // 512 Bytes per bit

    constexpr size_t CARDTABLE_LENGTH_IN_BYTE_PER_CHUNK = CHUNK_SIZE / CARDTABLE_SIZE_PER_BIT / 8;

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

        uint8_t         m_alloc_timing;
        uint8_t /* woomem_GCUnitTypeMask */ m_gc_type;
        uint8_t         m_gc_age;
        atomic_uint8_t  m_gc_marked;
        /* NOTE: Consider use this field to store unit offset, avoid using m_parent_page */
        uint16_t        _reserved_;
        uint16_t        m_next_alloc_unit_offset;

        WOOMEM_FORCE_INLINE void destroy() noexcept
        {
            if (m_gc_type & WOOMEM_GC_UNIT_TYPE_HAS_FINALIZER)
            {
                gc::g_global_gc_methods.m_destroyer(
                    gc::g_global_gc_methods.m_user_ctx,
                    reinterpret_cast<void*>(this + 1));
            }
        }
        WOOMEM_FORCE_INLINE bool try_free_this_unit_head() noexcept
        {
            if (WOOMEM_GC_MARKED_RELEASED != m_gc_marked.exchange(
                WOOMEM_GC_MARKED_RELEASED,
                memory_order::memory_order_relaxed))
            {
                destroy();
                return true;
            }
            return false;
        }
        WOOMEM_FORCE_INLINE void fast_free_unit_manually() noexcept
        {
            assert(WOOMEM_GC_MARKED_RELEASED != m_gc_marked.load(memory_order::memory_order_relaxed));

            m_gc_marked.store(WOOMEM_GC_MARKED_RELEASED, memory_order::memory_order_relaxed);
            destroy();
        }
        WOOMEM_FORCE_INLINE void init_unit_head() noexcept
        {
            /* unit_head->m_alloc_timing will be set when allocate. */
            m_gc_age = NEW_BORN_GC_AGE;
            m_gc_marked.store(
                WOOMEM_GC_MARKED_RELEASED, memory_order::memory_order_relaxed);
        }
        WOOMEM_FORCE_INLINE bool is_not_marked_during_this_round_gc(uint8_t gc_timing) const noexcept
        {
            return m_gc_marked.load(memory_order::memory_order_relaxed) == WOOMEM_GC_MARKED_UNMARKED
                && (m_gc_type & WOOMEM_GC_UNIT_TYPE_NEED_SWEEP)
                && m_gc_age != gc_timing;
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
            m_page_head.m_abondon_page_flag.store(0, memory_order::memory_order_relaxed);
            m_page_head.m_freed_unit_head_offset.store(0, memory_order::memory_order_relaxed);

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

                assert(WOOMEM_GC_MARKED_RELEASED == allocating_unit_head->m_gc_marked.load(
                    memory_order::memory_order_relaxed));

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
                0, memory_order::memory_order_acq_rel);

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
            assert(WOOMEM_GC_MARKED_RELEASED == freeing_unit_head->m_gc_marked.load(
                memory_order::memory_order_relaxed));

            freeing_unit_head->m_next_alloc_unit_offset =
                m_page_head.m_freed_unit_head_offset.load(
                    memory_order::memory_order_relaxed);

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
                    memory_order::memory_order_release,
                    memory_order::memory_order_relaxed))
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
            m_page_head.m_abondon_page_flag.store(0, memory_order::memory_order_relaxed);

            m_page_head.m_freed_unit_head_offset.store(0, memory_order::memory_order_relaxed);
            m_page_head.m_next_alloc_unit_head_offset = 0;

            m_unit_head.m_parent_page = nullptr;
            m_unit_head.init_unit_head();
        }
    };
    static_assert(sizeof(LargePageUnitHead) == 32,
        "LargePageUnitHead size must be correct.");

    struct Chunk
    {
        Chunk* m_last_chunk;

        atomic_size_t m_next_commiting_page_count;
        atomic_size_t m_commited_page_count;

        uint8_t m_multipage_offset[CHUNK_SIZE / PAGE_SIZE];
        atomic_uint8_t* const m_cardtable /* same as virtual address begin */;
        Page* const m_reserved_address_begin;
        Page* const m_reserved_address_end;

        static_assert(atomic_uint8_t::is_always_lock_free,
            "atomic_uint8_t must be lock free for m_cardtable");

        Chunk(void* reserved_address) noexcept
            : m_last_chunk(nullptr)
            , m_next_commiting_page_count{ 0 }
            , m_commited_page_count{ 0 }
            , m_cardtable(reinterpret_cast<atomic_uint8_t*>(reserved_address))
            , m_reserved_address_begin(reinterpret_cast<Page*>(m_cardtable + CARDTABLE_LENGTH_IN_BYTE_PER_CHUNK))
            , m_reserved_address_end(m_reserved_address_begin + PAGE_COUNT_PER_CHUNK)
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
                m_commited_page_count.load(memory_order::memory_order_relaxed);

            const int decommit_status = woomem_os_decommit_memory(
                m_cardtable,
                CARDTABLE_LENGTH_IN_BYTE_PER_CHUNK + commited_page_count * PAGE_SIZE);
            assert(decommit_status == 0);
            (void)decommit_status;

            const int release_status = woomem_os_release_memory(
                m_cardtable,
                CARDTABLE_LENGTH_IN_BYTE_PER_CHUNK + CHUNK_SIZE);
            assert(release_status == 0);
            (void)release_status;
        }

        /* OPTIONAL */ void* allocate_new_page_in_chunk(
            PageGroupType page_group, bool* out_page_run_out) noexcept
        {
            assert(page_group < TOTAL_GROUP_COUNT);

            const size_t need_group_count = PAGE_GROUP_NEED_PAGE_COUNTS[page_group];

            size_t new_page_index =
                m_next_commiting_page_count.load(memory_order::memory_order_relaxed);

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
                    memory_order::memory_order_relaxed,
                    memory_order::memory_order_relaxed))
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

                // Mark multipage offset.
                for (size_t page_idx = 0; page_idx < need_group_count; ++page_idx)
                {
                    m_multipage_offset[new_page_index + page_idx] =
                        static_cast<uint8_t>(page_idx);
                }

                // Wait until other threads finish committing.
                do
                {
                    size_t expected_commited = new_page_index;
                    if (m_commited_page_count.compare_exchange_weak(
                        expected_commited,
                        new_page_index + need_group_count,
                        memory_order::memory_order_release,
                        memory_order::memory_order_relaxed))
                    {
                        // Successfully updated commited_page_count
                        return &m_reserved_address_begin[new_page_index];
                    }
                    else if (m_next_commiting_page_count.load(memory_order::memory_order_relaxed) <= new_page_index)
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
                            memory_order::memory_order_relaxed);

                    if (m_next_commiting_page_count > new_page_index)
                    {
                        // Need rollback.
                        if (m_next_commiting_page_count.compare_exchange_weak(
                            current_commiting_page_count,
                            new_page_index,
                            memory_order::memory_order_relaxed))
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

            void* const reserved_address =
                woomem_os_reserve_memory(CHUNK_SIZE + CARDTABLE_LENGTH_IN_BYTE_PER_CHUNK);

            if (WOOMEM_LIKELY(
                reserved_address != nullptr
                && 0 == woomem_os_commit_memory(reserved_address, CARDTABLE_LENGTH_IN_BYTE_PER_CHUNK)))
            {
                return new (chunk_storage) Chunk(reserved_address);
            }

            // Cannot reserve virtual address or failed to commit basic cardtable for new chunk.
            if (reserved_address != nullptr)
                (void)woomem_os_release_memory(reserved_address, CHUNK_SIZE + CARDTABLE_LENGTH_IN_BYTE_PER_CHUNK);

            free(chunk_storage);
            return nullptr;
        }
    };

    struct HugeUnitHead
    {
        /*
        LAYOUT:
            |               Head                | Body   |  CardTable |
            | HugeUnitHead   LargePageUnitHead |        |             |
        */
        HugeUnitHead* m_next_huge_unit;

        size_t          m_fact_unit_size; /* used for realloc */
        size_t          m_aligned_unit_size;
        atomic_uint8_t* m_cardtable; /* point to body end */

        LargePageUnitHead       m_large_page_unit_head;
        /* body begin here. */

        WOOMEM_FORCE_INLINE void init_huge_unit_head_by_size(size_t aligned_unit_size) noexcept
        {
            // Align to 1 card table byte size (4k).
            assert(aligned_unit_size % (CARDTABLE_SIZE_PER_BIT * 8) == 0);

            m_aligned_unit_size = aligned_unit_size;

            static_assert(sizeof(atomic_uint8_t) == sizeof(char),
                "sizeof(atomic_uint8_t) == sizeof(char) expected.");
            m_cardtable =
                reinterpret_cast<atomic_uint8_t*>(this + 1) + aligned_unit_size;

            /*
            m_page_index_in_chunk is uselese for HugeUnit.
            */
            /* m_large_page_unit_head.m_page_head.m_page_index_in_chunk = 0 */;
            m_large_page_unit_head.init_for_large_page_unit(PageGroupType::HUGE);

            /*
            Reset cardtable masks.
            */
            memset(m_cardtable, 0, aligned_unit_size / CARDTABLE_SIZE_PER_BIT / 8);
        }
    };

    // TODO: Need impl, and consider if m_chunk_map cannot alloc memory,
    //       how to handle that case.
    class AddrToBlockFastLookupTable
    {
        RWSpin m_rwspin;
        map<void*, /* OPTIONAL */ Chunk*> m_chunk_map;
    public:
        void _reset()noexcept
        {
            // `_reset` only invoked while shutting-down, donot need to lock guard.
            m_chunk_map.clear();
        }
        WOOMEM_FORCE_INLINE void add_new_chunk(Chunk* new_chunk_instance) noexcept
        {
            lock_guard<RWSpin> g(m_rwspin);

            auto result = m_chunk_map.insert(
                make_pair(
                    new_chunk_instance->m_reserved_address_begin,
                    new_chunk_instance));

            (void)result;
            assert(result.second);
        }
        WOOMEM_FORCE_INLINE void add_huge_unit(HugeUnitHead* huge_unit) noexcept
        {
            lock_guard<RWSpin> g(m_rwspin);

            auto result = m_chunk_map.insert(
                make_pair(
                    huge_unit + 1, /* Data begin here. */
                    nullptr));

            (void)result;
            assert(result.second);
        }
        WOOMEM_FORCE_INLINE void remove_huge_unit(HugeUnitHead* huge_unit)
        {
            lock_guard<RWSpin> g(m_rwspin);
            (void)m_chunk_map.erase(huge_unit + 1);
        }

        /* OPTIONAL */ UnitHead* lookup_unit_head(void* may_valid_addr) noexcept
        {
            m_rwspin.lock_shared();

            auto fnd = m_chunk_map.upper_bound(may_valid_addr);

            if (fnd == m_chunk_map.begin())
            {
                m_rwspin.unlock_shared();
                return nullptr;
            }

            --fnd;
            void* const storage_begin = fnd->first;
            /* OPTIONAL */ Chunk* const storage_belong_chunk = fnd->second;

            m_rwspin.unlock_shared();

            if (storage_belong_chunk != nullptr)
            {
                if (may_valid_addr >= storage_belong_chunk->m_reserved_address_end)
                    return nullptr;

                size_t located_page_idx = static_cast<size_t>(
                    reinterpret_cast<intptr_t>(may_valid_addr)
                    - reinterpret_cast<intptr_t>(storage_belong_chunk->m_reserved_address_begin)) / PAGE_SIZE;

                located_page_idx -= storage_belong_chunk->m_multipage_offset[located_page_idx];

                Page* const unit_belong_page =
                    &storage_belong_chunk->m_reserved_address_begin[located_page_idx];

                if (unit_belong_page->m_page_head.m_page_belong_to_group >= LARGE_PAGES_1)
                {
                    // Large unit.
                    return &reinterpret_cast<LargePageUnitHead*>(unit_belong_page)->m_unit_head;
                }
                else
                {
                    assert(may_valid_addr > unit_belong_page
                        && unit_belong_page < unit_belong_page + 1);

                    const size_t unit_size_with_unit_head =
                        sizeof(UnitHead) +
                        UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[
                            unit_belong_page->m_page_head.m_page_belong_to_group];

                    const size_t located_unit_idx =
                        static_cast<size_t>(
                            reinterpret_cast<char*>(may_valid_addr) -
                            &unit_belong_page->m_entries[sizeof(PageHead)]) / unit_size_with_unit_head;

                    return reinterpret_cast<UnitHead*>(
                        &unit_belong_page->m_entries[
                            sizeof(PageHead) + located_unit_idx * unit_size_with_unit_head]);
                }
            }
            else
            {
                // Is huge unit?
                HugeUnitHead* const huge_unit_head =
                    reinterpret_cast<HugeUnitHead*>(storage_begin) - 1;

                if (static_cast<size_t>(
                    reinterpret_cast<char*>(may_valid_addr) -
                    reinterpret_cast<char*>(storage_begin)) < huge_unit_head->m_fact_unit_size)
                {
                    // In range.
                    return &huge_unit_head->m_large_page_unit_head.m_unit_head;
                }
                return nullptr;
            }

            // Never been here.
            abort();
        }
    };

    struct ThreadLocalPageCollection;

    struct GlobalPageCollection
    {
        RWSpin m_threads_mx;
        forward_list<ThreadLocalPageCollection*> m_threads;

        atomic<Chunk*> m_current_chunk;
        AddrToBlockFastLookupTable m_addr_to_chunk_table;

        atomic<Page*> m_free_group_page_list[FAST_AND_MIDIUM_GROUP_COUNT];

        // 用于储存可分配的大对象和巨型对象实例，注意，TOTAL_GROUP_COUNT 的前 
        // FAST_AND_MIDIUM_GROUP_COUNT 项并不使用，仅作占位（避免每次都减去这些值）。
        atomic<LargePageUnitHead*> m_free_large_unit_list[TOTAL_GROUP_COUNT];

        // HUGE 对象并不使用 Page 进行管理，释放操作也应当立即发生。
        // TODO: 需要考虑如何高效地，在有 HUGE 对象的情况下，能够快速校验地址是否合法。
        /*
        m_huge_units_for_gc_walk_through 用于储存当前已经分配出的所有巨大节点，在（且
        仅在）GC的释放操作完成后，由GC线程负责整理此列表，排除所有未被分配的节点。

        此表仅用于GC标记时，遍历所有尚处于存活状态的巨大节点，然后标记它们的 CardTable。
        因为 HUGE 单元的 CardTable 是独立的
        */
        atomic<HugeUnitHead*> m_huge_units_for_gc_walk_through;

        void reset() noexcept;

        /* OPTIONAL */ void* allocate_new_page(PageGroupType page_group)
        {
            Chunk* current_chunk =
                m_current_chunk.load(memory_order::memory_order_relaxed);

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
                            memory_order::memory_order_release,
                            memory_order::memory_order_acquire))
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

            auto* free_page = group.load(memory_order::memory_order_acquire);

            while (free_page != nullptr)
            {
                if (group.compare_exchange_weak(
                    free_page,
                    free_page->m_page_head.m_next_page,
                    memory_order::memory_order_acq_rel,
                    memory_order::memory_order_acquire))
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
                group.load(memory_order::memory_order_relaxed);

            while (!group.compare_exchange_weak(
                freeing_page->m_page_head.m_next_page,
                freeing_page,
                memory_order::memory_order_release,
                memory_order::memory_order_relaxed))
            {
                WOOMEM_PAUSE();
            }
        }

        void return_freed_large_unit_asyncly(LargePageUnitHead* large_unit_head) noexcept
        {
            assert(large_unit_head->m_page_head.m_page_belong_to_group != PageGroupType::HUGE);

            auto& large_unit_list = m_free_large_unit_list[
                large_unit_head->m_page_head.m_page_belong_to_group];

            large_unit_head->m_page_head.m_next_large_unit =
                large_unit_list.load(memory_order::memory_order_relaxed);
            while (!large_unit_list.compare_exchange_weak(
                large_unit_head->m_page_head.m_next_large_unit,
                large_unit_head,
                memory_order::memory_order_release,
                memory_order::memory_order_relaxed))
            {
                WOOMEM_PAUSE();
            }
        }
        /* OPTIONAL */ void* try_alloc_large_unit(PageGroupType group_type) noexcept
        {
            auto& large_unit_list = m_free_large_unit_list[group_type];
            LargePageUnitHead* free_large_unit =
                large_unit_list.load(memory_order::memory_order_acquire);
            while (free_large_unit != nullptr)
            {
                if (large_unit_list.compare_exchange_weak(
                    free_large_unit,
                    free_large_unit->m_page_head.m_next_large_unit,
                    memory_order::memory_order_acq_rel,
                    memory_order::memory_order_acquire))
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

        /* OPTIONAL */ HugeUnitHead* try_alloc_huge_unit_step1(size_t unit_size) noexcept
        {
            const size_t aligned_alloc_unit_size =
                (unit_size + (CARDTABLE_SIZE_PER_BIT * 8 - 1)) & ~(CARDTABLE_SIZE_PER_BIT * 8 - 1);

            HugeUnitHead* const new_allocated_huge_unit =
                reinterpret_cast<HugeUnitHead*>(
                    malloc(
                        // HugeUnitHead
                        sizeof(HugeUnitHead)
                        // UnitBody
                        + aligned_alloc_unit_size
                        // CardTable
                        + (aligned_alloc_unit_size / CARDTABLE_SIZE_PER_BIT / 8)));

            if (WOOMEM_LIKELY(new_allocated_huge_unit != NULL))
            {
                new_allocated_huge_unit->init_huge_unit_head_by_size(aligned_alloc_unit_size);
                new_allocated_huge_unit->m_fact_unit_size = unit_size;

                return new_allocated_huge_unit;
            }
            return nullptr;
        }
        void commit_huge_unit_into_list_step2(HugeUnitHead* huge_unit)
        {
            assert(WOOMEM_GC_MARKED_RELEASED != huge_unit->m_large_page_unit_head.m_unit_head.m_gc_marked.load(
                memory_order::memory_order_relaxed));

            huge_unit->m_next_huge_unit = m_huge_units_for_gc_walk_through.load(memory_order::memory_order_relaxed);
            while (!m_huge_units_for_gc_walk_through.compare_exchange_weak(
                huge_unit->m_next_huge_unit,
                huge_unit,
                memory_order::memory_order_release,
                memory_order::memory_order_relaxed))
            {
                WOOMEM_PAUSE();
            }
        }

        void register_thread(ThreadLocalPageCollection* thread_local_page_collection)noexcept
        {
            lock_guard<RWSpin> g(m_threads_mx);
            m_threads.push_front(thread_local_page_collection);
        }
        void unregister_thread(ThreadLocalPageCollection* thread_local_page_collection)noexcept
        {
            lock_guard<RWSpin> g(m_threads_mx);

            auto i = m_threads.before_begin();
            const auto e = m_threads.end();

            for (;;)
            {
                auto last_i = i++;

                if (last_i == e)
                    // Unexpected.
                    abort();

                if (*i == thread_local_page_collection)
                {
                    m_threads.erase_after(last_i);
                    break;
                }

            }
        }
    };
    GlobalPageCollection g_global_page_collection{};

    namespace gc
    {
        /*
        TODO: 优化 GlobalGrayList 实现，考虑一个节点储存复数个单元？
        */
        struct GlobalGrayList
        {
            struct GrayUnit
            {
                UnitHead* m_gray_marked_unit;
                GrayUnit* m_last;
            };

            atomic<GrayUnit*> m_list;
            atomic<GrayUnit*> m_dropped;

            GlobalGrayList()
                : m_list(nullptr)
                , m_dropped(nullptr)
            {

            }
            ~GlobalGrayList()
            {
                GrayUnit* current = m_list.load(std::memory_order_relaxed);
                while (current != nullptr)
                {
                    GrayUnit* next = current->m_last;
                    free(current);
                    current = next;
                }

                current = m_dropped.load(std::memory_order_relaxed);
                while (current != nullptr)
                {
                    GrayUnit* next = current->m_last;
                    free(current);
                    current = next;
                }
            }

            GlobalGrayList(const GlobalGrayList&) = delete;
            GlobalGrayList(GlobalGrayList&&) = delete;
            GlobalGrayList& operator =(const GlobalGrayList&) = delete;
            GlobalGrayList& operator =(GlobalGrayList&&) = delete;

            GrayUnit* _get_usable_node()
            {
                GrayUnit* dropped_node = m_dropped.load(std::memory_order_relaxed);
                if (dropped_node != nullptr)
                {
                    while (!m_dropped.compare_exchange_weak(
                        dropped_node,
                        dropped_node->m_last,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                    {
                        /* Retry. */
                        if (dropped_node == nullptr)
                            // Opus, list empty.
                            break;
                    }
                }

                if (dropped_node == nullptr)
                {
                    dropped_node = static_cast<GrayUnit*>(malloc(sizeof(GrayUnit)));
                    if (dropped_node == nullptr)
                        // Fatal error, cannot alloc memory.
                        abort();
                }
                return dropped_node;
            }

            void add(UnitHead* unit_head) noexcept
            {
                GrayUnit* unit = _get_usable_node();

                unit->m_gray_marked_unit = unit_head;
                unit->m_last = m_list.load(memory_order_relaxed);

                while (!m_list.compare_exchange_weak(
                    unit->m_last,
                    unit,
                    memory_order::memory_order_relaxed,
                    memory_order::memory_order_relaxed))
                {
                    WOOMEM_PAUSE();
                }
            }
            void drop_list(GrayUnit* drop_unit_list) noexcept
            {
                GrayUnit* last_of_dropping_list = drop_unit_list;
                do
                {
                    if (last_of_dropping_list->m_last == nullptr)
                        break;

                    last_of_dropping_list = last_of_dropping_list->m_last;

                } while (true);

                last_of_dropping_list->m_last =
                    m_dropped.load(memory_order::memory_order_relaxed);

                while (!m_dropped.compare_exchange_weak(
                    last_of_dropping_list->m_last,
                    drop_unit_list,
                    memory_order::memory_order_relaxed,
                    memory_order::memory_order_relaxed))
                {
                    /* retry. */
                }
            }
            GrayUnit* pick_all_units() noexcept
            {
                return m_list.exchange(
                    nullptr,
                    memory_order::memory_order_relaxed);
            }
        };
    }

    struct ThreadLocalPageCollection
    {
        bool        m_is_marking;

        /*
        m_alloc_timing 用于缓存线程局部的 GC 轮次，每次线程检查点时更新此值。
        */
        uint8_t     m_alloc_timing;

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
            uint8_t unit_type_mask) noexcept
        {
            allocated_unit->m_alloc_timing = m_alloc_timing;
            allocated_unit->m_gc_type = unit_type_mask;
            allocated_unit->m_gc_age = NEW_BORN_GC_AGE;

            // 最后设置 m_gc_marked，使用 release 确保之前的写入对其他线程可见
            // 
            // NOTE: 如果不使用 release 序，GC线程可能会在观测到 m_allocated_status 的同时
            //      无法正确读取到分配的元数据；这可能导致 GC 错误判断单元的分配时机导致错误
            //      释放
            //
            allocated_unit->m_gc_marked.store(
                WOOMEM_GC_MARKED_UNMARKED,
                memory_order::memory_order_release);
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
                    assert(WOOMEM_GC_MARKED_RELEASED
                        == allocated_unit_head->m_gc_marked.load(
                            memory_order::memory_order_relaxed));

                    init_allocated_unit(allocated_unit_head, unit_type_mask);
                }
                return allocated_unit;
            }
            else
            {
                HugeUnitHead* const allocated_huge_unit = g_global_page_collection.try_alloc_huge_unit_step1(unit_size);
                if (WOOMEM_LIKELY(allocated_huge_unit != nullptr))
                {
                    assert(allocated_huge_unit->m_large_page_unit_head.m_unit_head.m_parent_page == nullptr);
                    assert(WOOMEM_GC_MARKED_RELEASED
                        == allocated_huge_unit->m_large_page_unit_head.m_unit_head.m_gc_marked.load(
                            memory_order::memory_order_relaxed));

                    init_allocated_unit(
                        &allocated_huge_unit->m_large_page_unit_head.m_unit_head, unit_type_mask);

                    g_global_page_collection.commit_huge_unit_into_list_step2(allocated_huge_unit);
                    g_global_page_collection.m_addr_to_chunk_table.add_huge_unit(allocated_huge_unit);

                    return allocated_huge_unit + 1;
                }
                return nullptr;
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
                        next_page->m_page_head.m_abondon_page_flag.store(1, memory_order::memory_order_release);
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
                    g_global_page_collection.return_freed_large_unit_asyncly(large_unit_head);
                }
                // Or: HUGE 对象，分配标记已经解除；确实的释放操作将由GC负责，此处啥也不做
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

        void reset() noexcept
        {
            m_is_marking = false;
            m_alloc_timing = 0;
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
        void drop() noexcept
        {
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

        ThreadLocalPageCollection() noexcept
        {
            reset();
            g_global_page_collection.register_thread(this);
        }
        ~ThreadLocalPageCollection()
        {
            g_global_page_collection.unregister_thread(this);
            drop();
        }

        ThreadLocalPageCollection(const ThreadLocalPageCollection&) = delete;
        ThreadLocalPageCollection(ThreadLocalPageCollection&&) = delete;
        ThreadLocalPageCollection& operator=(const ThreadLocalPageCollection&) = delete;
        ThreadLocalPageCollection& operator=(ThreadLocalPageCollection&&) = delete;
    };
    thread_local ThreadLocalPageCollection t_tls_page_collection;

    void GlobalPageCollection::reset() noexcept
    {
        // Reset fast lookup table.
        m_addr_to_chunk_table._reset();

        HugeUnitHead* huge_unit =
            m_huge_units_for_gc_walk_through.exchange(nullptr, memory_order::memory_order_relaxed);

        while (huge_unit != nullptr)
        {
            HugeUnitHead* const current_unit = huge_unit;
            huge_unit = huge_unit->m_next_huge_unit;

            // Donot invoke destroy function when reset?
            //(void)current_unit->m_large_page_unit_head.m_unit_head.try_free_this_unit_head();
            free(current_unit);
        }

        for (auto& free_page_group_page : m_free_group_page_list)
            free_page_group_page.store(nullptr, memory_order::memory_order_relaxed);

        for (auto& free_large_unit_list : m_free_large_unit_list)
            m_free_large_unit_list->store(nullptr, memory_order::memory_order_relaxed);

        Chunk* current_chunk =
            m_current_chunk.load(memory_order::memory_order_acquire);

        assert(current_chunk != nullptr);

        do
        {
            Chunk* last_chunk = current_chunk->m_last_chunk;

            current_chunk->~Chunk();
            free(current_chunk);

            current_chunk = last_chunk;

        } while (current_chunk != nullptr);

        m_current_chunk.store(
            nullptr,
            memory_order::memory_order_release);

        // Reset all threads.
        lock_guard<RWSpin> g(m_threads_mx);

        for (auto* t : m_threads)
            t->reset();
    }

    namespace gc
    {
        struct GCMain
        {
            GCMain(const GCMain&) = delete;
            GCMain(GCMain&&) = delete;
            GCMain& operator = (const GCMain&) = delete;
            GCMain& operator = (GCMain&&) = delete;

            uint8_t             m_gc_timing;
            atomic_bool         m_gc_in_marking;

            bool                m_gc_main_thread_trigger;
            thread              m_gc_main_thread;
            mutex               m_gc_main_thread_trigger_mx;
            condition_variable  m_gc_main_thread_trigger_cv;

            GlobalGrayList      m_global_gray_marking_list;

            /*
            因为 m_addr_to_chunk_table 的快速查找表只能定位到单元，但是不会检查
            单元本身是否是一个已经分配的内存，到标记时自然能够发现。
            */
            static WOOMEM_FORCE_INLINE /* OPTIONAL */ UnitHead* _try_get_unit_head(
                void* may_addr) noexcept
            {
                return g_global_page_collection.m_addr_to_chunk_table.lookup_unit_head(
                    may_addr);
            }
            static size_t _get_unit_fact_size(UnitHead* unit_head) noexcept
            {
                if (WOOMEM_LIKELY(unit_head->m_parent_page != nullptr))
                {
                    // Is normal page.
                    return UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[
                        unit_head->m_parent_page->m_page_head.m_page_belong_to_group];
                }
                else
                {
                    // Is large or huge unit.
                    LargePageUnitHead* const large_page_unit_head =
                        reinterpret_cast<LargePageUnitHead*>(unit_head + 1) - 1;

                    if (WOOMEM_LIKELY(large_page_unit_head->m_page_head.m_page_belong_to_group != HUGE))
                    {
                        // Is large unit
                        return UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[
                            large_page_unit_head->m_page_head.m_page_belong_to_group];
                    }
                    else
                    {
                        // Hahaha...
                        HugeUnitHead* const huge_unit_head =
                            reinterpret_cast<HugeUnitHead*>(unit_head + 1) - 1;

                        return huge_unit_head->m_fact_unit_size;
                    }
                }
                // Never been here.
                abort();
            }

            void _mark_unit_to_gray(
                UnitHead* unit_head,
                vector<UnitHead*>* modified_gray_units_to_continue_mark)
            {
                uint8_t expected_state = WOOMEM_GC_MARKED_UNMARKED;
                if (unit_head->m_gc_marked.compare_exchange_strong(
                    expected_state,
                    WOOMEM_GC_MARKED_SELF_MARKED,
                    memory_order::memory_order_relaxed,
                    memory_order::memory_order_relaxed))
                {
                    // Marked, send to continue mark list.
                    modified_gray_units_to_continue_mark->push_back(unit_head);
                }
            }

            bool _pick_all_unit_in_ray_marking_list_and_mark_them(
                vector<UnitHead*>* modified_gray_units_to_continue_mark) noexcept
            {
                auto* const gray_units = m_global_gray_marking_list.pick_all_units();
                if (gray_units == nullptr)
                    // All unit in gray list has been marked.
                    return false;

                auto* current_gray_unit = gray_units;
                do
                {
                    // Mark unit to continue marking list.
                    _mark_unit_to_gray(
                        current_gray_unit->m_gray_marked_unit,
                        modified_gray_units_to_continue_mark);

                    // Go next.
                    current_gray_unit = current_gray_unit->m_last;

                } while (current_gray_unit != nullptr);

                // Drop back units.
                m_global_gray_marking_list.drop_list(gray_units);

                return true;
            }

            void _gc_main_job() noexcept
            {
                // 1. GC 开始，更新轮次计数
                ++m_gc_timing;
                m_gc_in_marking.store(true, memory_order::memory_order_release);

                // 2. 调用注册的 GC 开始回调函数，此回调函数将负责起始标记，并阻塞到根对象标记完成；
                if (g_global_gc_methods.m_root_marking != NULL)
                    g_global_gc_methods.m_root_marking(g_global_gc_methods.m_user_ctx);

                // 3. 标记 WOOMEM 的根对象
                // TODO;

                // 4. 根据 Cardtable，标记老年代对象中的新生代对象
                // TODO;

                // 5. 起始标记完成，收集此刻的 CollectorContext，进行 Fullmark
                /*
                    * fullmark 期间，其他线程可能因写屏障，继续向 CollectorContext 中追加待标记节点
                    * 以下情况可以发生待标记节点的追加：
                        1）尝试将一个未标记单元储存到 Fullmarked 内存区域中
                        2）尝试将一个新生代单元储存到老年代的内存区域中
                */
                vector<UnitHead*> continue_marking_list;
                while (_pick_all_unit_in_ray_marking_list_and_mark_them(
                    &continue_marking_list))
                {
                    vector<UnitHead*> current_marking_list;
                    current_marking_list.swap(continue_marking_list);

                    for (auto* unit_to_mark : current_marking_list)
                    {
                        uint8_t expected_state = WOOMEM_GC_MARKED_SELF_MARKED;
                        if (!unit_to_mark->m_gc_marked.compare_exchange_strong(
                            expected_state,
                            WOOMEM_GC_MARKED_FULL_MARKED,
                            memory_order::memory_order_relaxed,
                            memory_order::memory_order_relaxed))
                        {
                            // This unit has been marked as black or has been freed manually.
                            assert(expected_state == WOOMEM_GC_MARKED_FULL_MARKED
                                || expected_state == WOOMEM_GC_MARKED_RELEASED);
                            continue;
                        }

                        if (unit_to_mark->m_gc_type & WOOMEM_GC_UNIT_TYPE_AUTO_MARK)
                        {
                            // Need auto mark.
                            const size_t unit_fact_step = _get_unit_fact_size(unit_to_mark) / sizeof(void*);
                            void** const unit_storages = reinterpret_cast<void**>(unit_to_mark + 1);

                            for (size_t i = 0; i < unit_fact_step; ++i)
                            {
                                UnitHead* const child_unit_to_mark =
                                    _try_get_unit_head(unit_storages[i]);

                                if (child_unit_to_mark != nullptr)
                                    _mark_unit_to_gray(child_unit_to_mark, &continue_marking_list);
                            }
                        }
                        if (unit_to_mark->m_gc_type & WOOMEM_GC_UNIT_TYPE_HAS_MARKER)
                        {
                            // Need to mark by user callback.
                            g_global_gc_methods.m_marker(
                                g_global_gc_methods.m_user_ctx,
                                unit_to_mark + 1);
                        }
                    }
                }

                // 6. 全部标记完成，回收未被标记的单元
                m_gc_in_marking.store(false, memory_order::memory_order_relaxed);

                // Walkthrough all chunks and huge units.
                Chunk* current_chunk =
                    g_global_page_collection.m_current_chunk.load(memory_order::memory_order_relaxed);

                while (current_chunk != nullptr)
                {
                    const size_t committed_page_count =
                        current_chunk->m_commited_page_count.load(memory_order::memory_order_relaxed);

                    for (size_t pid = 0; pid < committed_page_count; ++pid)
                    {
                        Page* const current_page = &current_chunk->m_reserved_address_begin[pid];

                        assert(current_page->m_page_head.m_page_belong_to_group != HUGE);
                        if (current_page->m_page_head.m_page_belong_to_group < LARGE_PAGES_1)
                        {
                            // Is normal page.
                            const size_t page_unit_size_with_unit_head =
                                sizeof(UnitHead) + UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[
                                    current_page->m_page_head.m_page_belong_to_group];

                            for (
                                size_t boffset = sizeof(PageHead);
                                boffset < sizeof(Page);
                                boffset += page_unit_size_with_unit_head)
                            {
                                UnitHead* const this_unit_head = reinterpret_cast<UnitHead*>(
                                    &current_page->m_entries[boffset]);

                                assert(this_unit_head->m_parent_page == current_page);

                                if (this_unit_head->is_not_marked_during_this_round_gc(m_gc_timing))
                                {
                                    // Unit that need to be sweep didn't marked, and not allocated during this round GC scan.
                                    // Release it.

                                    this_unit_head->m_parent_page->free_unit_in_this_page_asyncly(this_unit_head);
                                }
                            }
                        }
                        else
                        {
                            // Is large unit page.
                            LargePageUnitHead* const this_large_unit_head =
                                reinterpret_cast<LargePageUnitHead*>(current_page);

                            assert(this_large_unit_head->m_unit_head.m_parent_page == nullptr);

                            if (this_large_unit_head->m_unit_head.is_not_marked_during_this_round_gc(m_gc_timing))
                            {
                                if (this_large_unit_head->m_unit_head.try_free_this_unit_head())
                                {
                                    g_global_page_collection.return_freed_large_unit_asyncly(this_large_unit_head);
                                }
                            }

                            // Move forward to skip current unit.
                            pid += PAGE_GROUP_NEED_PAGE_COUNTS[
                                current_page->m_page_head.m_page_belong_to_group] - 1;
                        }
                    }

                    current_chunk = current_chunk->m_last_chunk;
                }

                // Walkthrough all huge units.
                HugeUnitHead* current_huge_unit =
                    g_global_page_collection.m_huge_units_for_gc_walk_through.exchange(
                        nullptr, memory_order::memory_order_relaxed);

                while (current_huge_unit != nullptr)
                {
                    HugeUnitHead* const next_huge_unit = current_huge_unit->m_next_huge_unit;

                    if (current_huge_unit->m_large_page_unit_head.m_unit_head
                        .is_not_marked_during_this_round_gc(m_gc_timing)
                        || current_huge_unit->m_large_page_unit_head.m_unit_head.m_gc_marked
                        .load(memory_order_relaxed) == WOOMEM_GC_MARKED_RELEASED)
                    {
                        g_global_page_collection.m_addr_to_chunk_table.remove_huge_unit(current_huge_unit);
                        free(current_huge_unit);
                    }
                    else
                    {
                        // Dropback
                        g_global_page_collection.commit_huge_unit_into_list_step2(current_huge_unit);
                    }

                    current_huge_unit = next_huge_unit;
                }
            }
            void _gc_main_thread() noexcept
            {
                for (;;)
                {
                    unique_lock<mutex> ug(m_gc_main_thread_trigger_mx);
                    for (size_t i = 0; i < 1000; ++i)
                    {
                        m_gc_main_thread_trigger_cv.wait_for(
                            ug,
                            10ms,   // Force trigger GC per 10sec.
                            [this]()
                            {
                                // TODO: 在此检查 GC 策略
                                return m_gc_main_thread_trigger;
                            });
                    }
                    ug.unlock();

                    // GC Job here.
                    _gc_main_job();
                }
            }

            GCMain()
                : m_gc_timing(0)
                , m_gc_main_thread_trigger(false)
            {
                m_gc_in_marking.store(false, memory_order::memory_order_relaxed);
                m_gc_main_thread = thread(&GCMain::_gc_main_thread, this);
            }
            ~GCMain()
            {
                m_gc_main_thread.join();
            }

            void trigger() noexcept
            {
                lock_guard<mutex> ug(m_gc_main_thread_trigger_mx);
                m_gc_main_thread_trigger = true;

                m_gc_main_thread_trigger_cv.notify_one();
            }

            void sending_to_mark_gray(intptr_t may_addr) noexcept
            {
                /* OPTIONAL */ UnitHead* const unit_head =
                    _try_get_unit_head(reinterpret_cast<void*>(may_addr));

                if (unit_head != nullptr)
                    // TODO: Check if addr is in blocks?
                    m_global_gray_marking_list.add(unit_head);
            }

            /*
            fastcheck_and_sending_to_mark_gray 被用于检查和准备标记一些指向单元开头的
            指针；此处将使用掩码检查对齐情况，注意：如果一个指针指向一个分配空间的中间
            而非开头，不能使用此方法标记，这可能导致这些单元被错误地过滤掉。
            */
            void fastcheck_and_sending_to_mark_gray(intptr_t may_addr) noexcept
            {
                if (may_addr == 0 || may_addr & (BASE_ALIGNMENT - 1))
                    // Bad address, not a valid unit head.
                    return;

                sending_to_mark_gray(may_addr);
            }
        };
        GCMain* g_gc_main = nullptr;

        void init()
        {
            assert(g_gc_main == nullptr);
            g_gc_main = new GCMain();
        }
        void shutdown()
        {
            assert(g_gc_main != nullptr);

            delete g_gc_main;
            g_gc_main = nullptr;
        }
    }
}

using namespace woomem_cppimpl;

void woomem_init(
    woomem_UserContext user_ctx,
    woomem_MarkCallbackFunc marker,
    woomem_DestroyCallbackFunc destroyer,
    woomem_RootMarkingFunc root_marking)
{
    assert(g_global_page_collection.m_current_chunk.load(
        memory_order::memory_order_acquire) == nullptr);

    const size_t sys_mem_page_size = woomem_os_page_size();
    if (PAGE_SIZE % sys_mem_page_size != 0)
    {
        fprintf(stderr,
            "WOOMEM: Page size error. Alignment with the system page size is required.");
        abort();
    }
    if (CARDTABLE_LENGTH_IN_BYTE_PER_CHUNK % sys_mem_page_size != 0)
    {
        fprintf(stderr,
            "WOOMEM: Card table size error. Alignment with the system page size is required.");
        abort();
    }

    gc::g_global_gc_methods.m_user_ctx = user_ctx;
    gc::g_global_gc_methods.m_marker = marker;
    gc::g_global_gc_methods.m_destroyer = destroyer;
    gc::g_global_gc_methods.m_root_marking = root_marking;

    g_global_page_collection.m_current_chunk.store(
        Chunk::create_new_chunk(), memory_order::memory_order_release);

    // GC start.
    gc::init();
}

void woomem_shutdown(void)
{
    g_global_page_collection.reset();

    // GC stop.
    gc::shutdown();
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
            if (WOOMEM_LIKELY(old_group_type != PageGroupType::HUGE))
                return ptr;
            else
            {
                // HUGE 单元的尺寸规则和其他单元不大一样，需要特别处理一下
                HugeUnitHead* const old_huge_unit_head =
                    reinterpret_cast<HugeUnitHead*>(ptr) - 1;

                if (new_size <= old_huge_unit_head->m_aligned_unit_size)
                {
                    // Ok, 足够容纳，跳过
                    old_huge_unit_head->m_fact_unit_size = new_size;
                    return ptr;
                }
            }

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
        : (reinterpret_cast<HugeUnitHead*>(ptr) - 1)->m_fact_unit_size;

    // 复制数据：复制 min(old_unit_size, new_size) 字节
    memcpy(new_ptr, ptr, (old_unit_size < new_size) ? old_unit_size : new_size);

    // 释放旧内存
    woomem_free(ptr);

    return new_ptr;
}

void woomem_free(void* ptr)
{
    t_tls_page_collection.free(ptr);
}

void woomem_try_mark_unit_head(intptr_t address_may_invalid)
{
    gc::g_gc_main->fastcheck_and_sending_to_mark_gray(
        address_may_invalid);
}

void woomem_try_mark_unit(intptr_t address_may_invalid)
{
    gc::g_gc_main->sending_to_mark_gray(
        address_may_invalid);
}

woomem_Bool woomem_checkpoint(void)
{
    if (gc::g_gc_main->m_gc_in_marking.load(memory_order::memory_order_acquire))
    {
        // Sync GC timing.
        t_tls_page_collection.m_alloc_timing = gc::g_gc_main->m_gc_timing;
        t_tls_page_collection.m_is_marking = true;

        return WOOMEM_BOOL_TRUE;
    }
    else
    {
        // Sync GC state.
        t_tls_page_collection.m_is_marking = false;
    }

    return WOOMEM_BOOL_FALSE;
}

void woomem_write_barrier(void* writing_target_unit, void* addr)
{
    if (t_tls_page_collection.m_is_marking)
    {
        UnitHead* const writing_target_unit_head =
            reinterpret_cast<UnitHead*>(writing_target_unit) - 1;

        // TODO: 怎么有效地检查正在写入 Black Unit，需要仔细考虑
        /*if (writing_target_unit_head->m_gc_marked.load()*/
    }
}