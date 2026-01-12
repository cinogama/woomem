#include "woomem.h"
#include "woomem_os_mmap.h"

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <atomic>
#include <new>
#include <thread>
#include <vector>
#include <algorithm>

using namespace std;

#if defined(__GNUC__) || defined(__clang__)
#   define WOOMEM_LIKELY(x)       __builtin_expect(!!(x), 1)
#   define WOOMEM_UNLIKELY(x)     __builtin_expect(!!(x), 0)
#   define WOOMEM_FORCE_INLINE    __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#   define WOOMEM_LIKELY(x)       (x)
#   define WOOMEM_UNLIKELY(x)     (x)
#   define WOOMEM_FORCE_INLINE    __forceinline
#else
#   define WOOMEM_LIKELY(x)       (x)
#   define WOOMEM_UNLIKELY(x)     (x)
#   define WOOMEM_ALWAYS_INLINE   inline
#   define WOOMEM_NOINLINE
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

    // SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[(AllocSize + 7) >> 3]
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

    WOOMEM_FORCE_INLINE PageGroupType get_page_group_type_for_size(size_t size)
    {
        if (WOOMEM_LIKELY(size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[PageGroupType::SMALL_1024]))
        {
            return SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[(size + 7) >> 3];
        }
        else if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_65504])
        {
            if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_1440])
                return MIDIUM_1440;
            else if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_2168])
                return MIDIUM_2168;
            else if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_3104])
                return MIDIUM_3104;
            else if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_4352])
                return MIDIUM_4352;
            else if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_6536])
                return MIDIUM_6536;
            else if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_9344])
                return MIDIUM_9344;
            else if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_13088])
                return MIDIUM_13088;
            else if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_21824])
                return MIDIUM_21824;
            else if (size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_32744])
                return MIDIUM_32744;
            else /* size <= 65504 */
                return MIDIUM_65504;
        }
        else
        {
            return LARGE;
        }
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
    };
    static_assert(sizeof(UnitHead) == 16 && alignof(UnitHead) == 8,
        "UnitHead size and alignment must be correct.");

    union Page
    {
        char m_entries[PAGE_SIZE];

        struct
        {
            PageHead m_page_head;

            // Followed by many UnitHead + User Data.
            char m_storage[PAGE_SIZE - sizeof(PageHead)];
        };

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
        void free_unit_in_this_page_asyncly(UnitHead* freeing_unit_head) noexcept
        {
            assert(freeing_unit_head->m_parent_page == this);
            assert(0 != freeing_unit_head->m_allocated_status.load(std::memory_order_relaxed));

            uint8_t expected_status = 1;
            if (WOOMEM_LIKELY(
                freeing_unit_head->m_allocated_status.compare_exchange_strong(
                    expected_status,
                    0,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)))
            {
                // Ok, this unit is freed by current thread now
                const uint16_t current_unit_offset =
                    static_cast<uint16_t>(
                        reinterpret_cast<char*>(freeing_unit_head) -
                        m_entries);

                assert(current_unit_offset % 8 == 0);

                // Reset GC attribute, avoid set them in allocation path.
                freeing_unit_head->m_gc_age = NEW_BORN_GC_AGE;
                freeing_unit_head->m_gc_marked.store(
                    WOOMEM_GC_MARKED_UNMARKED, std::memory_order_relaxed);

                freeing_unit_head->m_next_alloc_unit_offset =
                    m_page_head.m_freed_unit_head_offset.load(
                        std::memory_order_acquire);
                while (
                    !m_page_head.m_freed_unit_head_offset.compare_exchange_weak(
                        freeing_unit_head->m_next_alloc_unit_offset,
                        current_unit_offset,
                        std::memory_order_release,
                        std::memory_order_acquire))
                {
                    // Do nothing.
                    std::this_thread::yield();
                }

                assert(freeing_unit_head->m_next_alloc_unit_offset % 8 == 0);

                // Count for page reuses.
                // NOTE: Use release order to make sure count operation always happend 
                //      after the free operation.
                (void)m_page_head.m_free_times.fetch_add(1, std::memory_order_release);

                return;
            }
            // Else: this unit might be freed by GC, or double free detected.
            assert(expected_status == 0);
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
                // Retry updating last chunk.
                std::this_thread::yield();
            }
            return new_chunk;
        }

        static /* OPTIONAL */ Page* allocate_new_page(PageGroupType page_group)
        {
            Chunk* current_chunk =
                g_current_chunk.load(std::memory_order_relaxed);

            do
            {
                if (WOOMEM_LIKELY(current_chunk != nullptr))
                {
                    bool page_run_out;
                    Page* new_page = current_chunk->allocate_new_page_in_chunk(
                        page_group, &page_run_out);

                    if (WOOMEM_UNLIKELY(page_run_out))
                    {
                        // Check last chunk?
                        current_chunk = current_chunk->m_last_chunk;
                        continue;
                    }

                    // new_page might be nullptr if commit memory failed.
                    return new_page;
                }
                else
                {
                    current_chunk = create_new_chunk();
                    if (WOOMEM_LIKELY(current_chunk))
                        continue;

                    // Failed to alloc chunk..
                    return nullptr;
                }

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

            auto* free_page = group.load(std::memory_order_relaxed);

            if (free_page != nullptr)
            {
                while (!group.compare_exchange_weak(
                    free_page,
                    free_page->m_page_head.m_next_page,
                    std::memory_order_release,
                    std::memory_order_relaxed))
                {
                    if (free_page == nullptr)
                        break;
                }
            }

            if (free_page == nullptr)
            {
                // Try allocate a new page from Chunk.
                // NOTE: `m_next_page` has been set to nullptr in allocate_new_page.
                return Chunk::allocate_new_page(group_type);
            }

            free_page->m_page_head.m_next_page = nullptr;
            return free_page;
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
                std::this_thread::yield();
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
        Page* m_current_allocating_page_for_group[TOTAL_GROUP_COUNT];

        void* alloc(size_t unit_size, woomem_GCUnitType unit_type) noexcept
        {
            const auto alloc_group = get_page_group_type_for_size(unit_size);
            if (WOOMEM_LIKELY(alloc_group != PageGroupType::LARGE))
            {
                auto*& current_alloc_group = m_current_allocating_page_for_group[alloc_group];
            _lable_alloc_successfully:
                auto* const unit = current_alloc_group->try_allocate_unit_from_page();
                if (WOOMEM_LIKELY(unit != nullptr))
                {
                    // Init unit attribute before m_allocated_status is set to 1.
                    unit->m_alloc_timing =
                        m_alloc_timing & static_cast<uint8_t>(0b01111u);
                    unit->m_gc_type =
                        static_cast<uint8_t>(unit_type);

                    // Unit allocated.
                    unit->m_allocated_status.store(1, std::memory_order_release);

                    return unit + 1;
                }
                else
                {
                    // Page run out, try next page in this group.
                    auto* const next_page =
                        current_alloc_group->m_page_head.m_next_page;

                    if (WOOMEM_LIKELY(next_page->try_tidy_page()))
                    {
                        current_alloc_group = next_page;
                        goto _lable_alloc_successfully;
                    }

                    // This page run out, drop it and try get new pages from global pool.
                    auto* const new_page =
                        g_global_page_collection.try_get_free_page(alloc_group);

                    if (WOOMEM_LIKELY(new_page != nullptr))
                    {
                        // Got a new page from global pool.
                        current_alloc_group->m_page_head.m_next_page = new_page;
                        new_page->m_page_head.m_next_page = next_page->m_page_head.m_next_page;

                        next_page->m_page_head.m_abondon_page_flag.store(
                            1, std::memory_order_release);

                        current_alloc_group = new_page;
                        goto _lable_alloc_successfully;
                    }

                    // We have tried all method to get new unit, alloc failed...
                    return nullptr;
                }
            }
            else
            {
                // TODO: Large alloc
                abort();
            }
        }

        ThreadLocalPageCollection() noexcept
            : m_alloc_timing{ 0 }
        {
            for (PageGroupType t = PageGroupType::SMALL_8;
                t < PageGroupType::LARGE;
                t = static_cast<PageGroupType>(static_cast<uint8_t>(t + 1)))
            {
                Page* reserved_pages[THREAD_LOCAL_POOL_MAX_CACHED_PAGE_COUNT_PER_GROUP];

                for (Page*& page : reserved_pages)
                {
                    page = g_global_page_collection.try_get_free_page(t);
                    if (WOOMEM_UNLIKELY(page == nullptr))
                    {
                        // Oh... wtf...

                        // TODO: Solve this problem later.
                        abort();
                    }
                }

                for (size_t i = 0; i < THREAD_LOCAL_POOL_MAX_CACHED_PAGE_COUNT_PER_GROUP; ++i)
                {
                    reserved_pages[i]->m_page_head.m_next_page =
                        reserved_pages[(i + 1) % THREAD_LOCAL_POOL_MAX_CACHED_PAGE_COUNT_PER_GROUP];
                }

                m_current_allocating_page_for_group[t] = reserved_pages[0];
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
                Page* const first_freeing_page = m_current_allocating_page_for_group[t];
                Page* freeing_page = first_freeing_page;
                do
                {
                    Page* const next_page = freeing_page->m_page_head.m_next_page;
                    g_global_page_collection.return_free_page(freeing_page);
                    freeing_page = next_page;

                } while (freeing_page != first_freeing_page);
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
}
void woomem_shutdown(void)
{
    Chunk* current_chunk =
        Chunk::g_current_chunk.load(std::memory_order_acquire);

    while (current_chunk != nullptr)
    {
        Chunk* last_chunk = current_chunk->m_last_chunk;

        current_chunk->~Chunk();
        free(current_chunk);

        current_chunk = last_chunk;
    }

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

/* OPTIONAL */ void woomem_free(void* ptr)
{
    Page::free_page_unit_asyncly(ptr);
}