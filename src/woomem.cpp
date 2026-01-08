#include "woomem.h"
#include "woomem_os_mmap.h"

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <atomic>
#include <new>
#include <mutex>
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
        SMALL_8B,
        SMALL_16B,
        SMALL_24B,
        SMALL_32B,
        SMALL_48B,
        SMALL_64B,
        SMALL_80B,
        SMALL_96B,
        SMALL_112B,
        SMALL_128B,
        SMALL_192B,
        SMALL_256B,
        SMALL_384B,
        SMALL_512B,
        SMALL_768B,
        SMALL_1032B,

        MEDIUM_2176B,
        MEDIUM_4360B,
        MEDIUM_9352B,
        MEDIUM_21832B,
        MEDIUM_65520B,

        LARGE,

        TOTAL_GROUP_COUNT,
    };
    constexpr size_t UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[] =
    {
        8, 
        16, 
        24, 
        32,
        48, 
        64, 
        80, 
        96,
        112, 
        128, 
        192, 
        256, 
        384, 
        512, 
        768, 
        1032,
        2176, 
        4360,
        9352, 
        21832,
        65520,
    };
    static_assert(UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[PageGroupType::SMALL_1032B] == 1032,
        "UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP must be correct.");
    static_assert(UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP[PageGroupType::MEDIUM_65520B] == 65520,
        "UINT_SIZE_FOR_PAGE_GROUP_TYPE_FAST_LOOKUP must be correct.");

    constexpr size_t MAX_SMALL_UNIT_SIZE = 1032;
    constexpr size_t SMALL_UNIT_FAST_LOOKUP_TABLE_SIZE =
        (MAX_SMALL_UNIT_SIZE + 7) / 8 + 1;

    // SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[(AllocSize + 7) >> 3]
    constexpr PageGroupType SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[SMALL_UNIT_FAST_LOOKUP_TABLE_SIZE] =
    {
        SMALL_8B,   // 0
        SMALL_8B,   // 8
        SMALL_16B,  // 16
        SMALL_24B,  // 24
        SMALL_32B,  // 32
        SMALL_48B, SMALL_48B,   // 40, 48
        SMALL_64B, SMALL_64B,    // 56, 64
        SMALL_80B, SMALL_80B,   // 72, 80
        SMALL_96B, SMALL_96B,   // 88, 96
        SMALL_112B, SMALL_112B, // 104, 112
        SMALL_128B, SMALL_128B, // 120, 128
        SMALL_192B, SMALL_192B, SMALL_192B, SMALL_192B,
        SMALL_192B, SMALL_192B, SMALL_192B, SMALL_192B, // 136..192
        SMALL_256B, SMALL_256B, SMALL_256B, SMALL_256B,
        SMALL_256B, SMALL_256B, SMALL_256B, SMALL_256B, // 200..256
        SMALL_384B, SMALL_384B, SMALL_384B, SMALL_384B,
        SMALL_384B, SMALL_384B, SMALL_384B, SMALL_384B,
        SMALL_384B, SMALL_384B, SMALL_384B, SMALL_384B,
        SMALL_384B, SMALL_384B, SMALL_384B, SMALL_384B, // 264..384
        SMALL_512B, SMALL_512B, SMALL_512B, SMALL_512B,
        SMALL_512B, SMALL_512B, SMALL_512B, SMALL_512B,
        SMALL_512B, SMALL_512B, SMALL_512B, SMALL_512B,
        SMALL_512B, SMALL_512B, SMALL_512B, SMALL_512B, // 392..512
        SMALL_768B, SMALL_768B, SMALL_768B, SMALL_768B,
        SMALL_768B, SMALL_768B, SMALL_768B, SMALL_768B,
        SMALL_768B, SMALL_768B, SMALL_768B, SMALL_768B,
        SMALL_768B, SMALL_768B, SMALL_768B, SMALL_768B,
        SMALL_768B, SMALL_768B, SMALL_768B, SMALL_768B,
        SMALL_768B, SMALL_768B, SMALL_768B, SMALL_768B,
        SMALL_768B, SMALL_768B, SMALL_768B, SMALL_768B,
        SMALL_768B, SMALL_768B, SMALL_768B, SMALL_768B, // 520..768
        SMALL_1032B, SMALL_1032B, SMALL_1032B, SMALL_1032B,
        SMALL_1032B, SMALL_1032B, SMALL_1032B, SMALL_1032B,
        SMALL_1032B, SMALL_1032B, SMALL_1032B, SMALL_1032B,
        SMALL_1032B, SMALL_1032B, SMALL_1032B, SMALL_1032B,
        SMALL_1032B, SMALL_1032B, SMALL_1032B, SMALL_1032B,
        SMALL_1032B, SMALL_1032B, SMALL_1032B, SMALL_1032B,
        SMALL_1032B, SMALL_1032B, SMALL_1032B, SMALL_1032B,
        SMALL_1032B, SMALL_1032B, SMALL_1032B, SMALL_1032B,
        SMALL_1032B, // 776..MAX_SMALL_UNIT_SIZE
    };
    static_assert(sizeof(SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B) == (MAX_SMALL_UNIT_SIZE + 7) / 8 + 1,
        "SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B used for alloc(<= MAX_SMALL_UNIT_SIZE).");
    static_assert(SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[(MAX_SMALL_UNIT_SIZE + 7) / 8] == SMALL_1032B,
        "SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B must be filled correctly.");

    WOOMEM_FORCE_INLINE PageGroupType get_page_group_type_for_size(size_t size)
    {
        if (WOOMEM_LIKELY(size <= 1024))
        {
            return SMALL_PAGE_GROUPS_FAST_LOOKUP_FOR_EACH_8B[(size + 7) >> 3];
        }
        else
        {
            if (WOOMEM_LIKELY(size <= 65520))
            {
                if (size <= 2176)
                    return MEDIUM_2176B;
                else if (size <= 4360)
                    return MEDIUM_4360B;
                else if (size <= 9352)
                    return MEDIUM_9352B;
                else if (size <= 21832)
                    return MEDIUM_21832B;
                else
                    return MEDIUM_65520B;
            }
            else
            {
                return LARGE;
            }
        }
    }

    struct PageHead
    {
        alignas(8) PageGroupType m_page_belong_to_group;

        atomic_uint16_t m_freed_unit_head_offset;
        uint16_t m_next_alloc_unit_head_offset;
    };
    static_assert(sizeof(PageHead) == 8 && alignof(PageHead) == 8,
        "PageHead size and alignment must be correct.");

    struct UnitHead
    {
        alignas(8) atomic_uint8_t m_allocated_status;
        atomic<woomem_MemoryAttribute> 
            m_attribute;

        atomic_uint16_t 
            m_next_free_unit_offset;
        uint16_t 
            m_next_alloc_unit_offset;
    };
    static_assert(atomic<woomem_MemoryAttribute>::is_always_lock_free,
        "atomic<woomem_MemoryAttribute> must be lock free for performance");
    static_assert(sizeof(UnitHead) == 8 && alignof(UnitHead) == 8,
        "UnitHead size and alignment must be correct.");

    union Page
    {
        char m_entries[PAGE_SIZE];
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
                        new_chunk->m_last_chunk,
                        new_chunk,
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

    // Will be inited in `woomem_init`
    atomic<Chunk*> Chunk::g_current_chunk;
}
