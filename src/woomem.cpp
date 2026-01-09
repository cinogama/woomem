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

        TOTAL_GROUP_COUNT,
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
        else
        {
            if (WOOMEM_LIKELY(size <= UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[MIDIUM_65504]))
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
    }

    struct PageHead
    {
        PageHead* m_next_freed_page;

        PageGroupType m_page_belong_to_group;

        atomic_uint16_t m_freed_unit_head_offset;
        uint16_t m_next_alloc_unit_head_offset;
    };
    static_assert(sizeof(PageHead) == 16 && alignof(PageHead) == 8,
        "PageHead size and alignment must be correct.");

    union Page;
    struct UnitHead
    {
        /* Used for user free. */
        Page* m_parent_page;

        atomic_uint8_t m_allocated_status; // 0 = freed, 1 = allocated
        atomic<woomem_MemoryAttribute> m_attribute;

        uint16_t m_next_alloc_unit_offset;
    };
    static_assert(atomic<woomem_MemoryAttribute>::is_always_lock_free,
        "atomic<woomem_MemoryAttribute> must be lock free for performance");
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

        void reinit_page_with_group(PageGroupType group_type, uint16_t first_unit_offset) noexcept
        {
            // Only empty and new page can be reinit.
            assert(group_type != PageGroupType::LARGE);

            m_page_head.m_page_belong_to_group = group_type;
            m_page_head.m_freed_unit_head_offset.store(0, std::memory_order_relaxed);

            const size_t unit_take_size_unit =
                UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[group_type] + sizeof(UnitHead);

            uint16_t* next_unit_offset_ptr =
                &m_page_head.m_next_alloc_unit_head_offset;

            for (uint16_t current_unit_head_begin = sizeof(PageHead);
                static_cast<size_t>(current_unit_head_begin) + unit_take_size_unit <= PAGE_SIZE;
                current_unit_head_begin += static_cast<uint16_t>(unit_take_size_unit))
            {
                *next_unit_offset_ptr = current_unit_head_begin;

                // Init unit head here.
                UnitHead* unit_head =
                    reinterpret_cast<UnitHead*>(&m_storage[current_unit_head_begin]);

                unit_head->m_parent_page = this;
                unit_head->m_allocated_status.store(0, std::memory_order_relaxed);

                next_unit_offset_ptr = &unit_head->m_next_alloc_unit_offset;

                // Donot need init m_attribute here.
                /* unit_head->m_attribute; */
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

                assert(0 == allocating_unit_head->m_allocated_status.load(std::memory_order_relaxed));

                /*
                ATTENTION: Attribute and allocated flag will be set after 
                        this function returns.
                */

                m_page_head.m_next_alloc_unit_head_offset =
                    allocating_unit_head->m_next_alloc_unit_offset;

                return allocating_unit_head;
            }
            return nullptr;
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
                freeing_unit_head->m_next_alloc_unit_offset =
                    m_page_head.m_freed_unit_head_offset.load(
                        std::memory_order_relaxed);

                const uint16_t current_unit_offset =
                    static_cast<uint16_t>(
                        reinterpret_cast<char*>(freeing_unit_head) -
                        m_entries);

                do
                {
                    // Do nothing.

                } while (
                    !m_page_head.m_freed_unit_head_offset.compare_exchange_weak(
                        freeing_unit_head->m_next_alloc_unit_offset,
                        current_unit_offset,
                        std::memory_order_release,
                        std::memory_order_relaxed));
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

        /* OPTIONAL */ Page* allocate_new_page_in_chunk(bool* out_page_run_out) noexcept
        {
            size_t new_page_index =
                m_next_commiting_page_count.fetch_add(1, std::memory_order_relaxed);

            if (new_page_index < CHUNK_SIZE / PAGE_SIZE)
            {
                *out_page_run_out = false;

                const auto status = woomem_os_commit_memory(
                    &m_reserved_address_begin[new_page_index],
                    PAGE_SIZE);

                if (status == 0)
                {
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

        static /* OPTIONAL */ Page* allocate_new_page()
        {
            Chunk* current_chunk =
                g_current_chunk.load(std::memory_order_relaxed);

            do
            {
                bool page_run_out;
                Page* new_page = current_chunk->allocate_new_page_in_chunk(&page_run_out);

                if (WOOMEM_UNLIKELY(page_run_out))
                {
                    // Check last chunk?
                    current_chunk = current_chunk->m_last_chunk;

                    if (WOOMEM_LIKELY(current_chunk != nullptr))
                        // Continue to try last chunk.
                        continue;

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

                    current_chunk = new (chunk_storage) Chunk(reserved_address);
                    current_chunk->m_last_chunk = g_current_chunk.load(std::memory_order_relaxed);

                    while (!g_current_chunk.compare_exchange_weak(
                        current_chunk->m_last_chunk,
                        current_chunk,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                    {
                        // Retry updating last chunk.
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
        union FreePageOrLargeUnit
        {
            atomic<Page*> m_free_page;
        };
        FreePageOrLargeUnit m_group_free_pages_and_large_unit[TOTAL_GROUP_COUNT];

    };

    // Will be inited in `woomem_init`
    atomic<Chunk*> Chunk::g_current_chunk;
}
