#include "woomem.h"
#include "woomem_os_mmap.h"

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <atomic>
#include <new>

using namespace std;

namespace woomem_cppimpl
{
    // 内存管理器的块大小，是 WooMem 的内存管理器向操作系统申请的保留
    // 地址空间的单位，32位系统的块大小是 256MB，64 位系统是 1GB。无论
    // 如何，MEMORY_CHUNK_SIZE 应当是 MEMORY_PAGE_SIZE 的整数倍
    constexpr size_t MEMORY_CHUNK_SIZE =
        sizeof(void*) == 4 ? (256 * 1024 * 1024) : (1024 * 1024 * 1024);

    // 内存管理器的页面大小，是 WooMem 向操作系统声明保留内存时的最小
    // 提交单位，由于系统提交映射的限制，此值必须是系统页面大小的整数
    // 倍（将在分配器初始化时运行时检查）。
    constexpr size_t MEMORY_PAGE_SIZE = 64 * 1024;
    static_assert(
        MEMORY_CHUNK_SIZE% MEMORY_PAGE_SIZE == 0,
        "MEMORY_CHUNK_SIZE must be multiple of MEMORY_PAGE_SIZE");

    constexpr size_t MEMORY_PAGES_PER_CHUNK =
        MEMORY_CHUNK_SIZE / MEMORY_PAGE_SIZE;

    // 分配基础对齐
    constexpr size_t MEMORY_UNIT_BASE_ALIGN = 8;

    /*
    WooMem 内存管理逻辑

    内存分配器由一系列的 块(Chunk) 构成，一个块包含若干个 页面(Page)，
    当块完全分配完毕之后，内存分配器将尝试申请一个新的块。每个页面
    包含若干个 内存单元(Memory Unit)，页面有自己的独有划分大小，一旦
    一个页面被划分为某个大小的内存单元后，该页面只能分配该大小的内存单
    元（除非此页面在某轮 GC 之后被完全释放）。
    */

    struct PageHeader
    {
        uint16_t m_unit_size_in_this_page;
        uint16_t m_total_unit_count_in_this_page;
        
        // Free operation might happend in multi-threaded environment.
        // `m_freed_unit_offset` 储存刚刚被释放的内存单元的偏移量，
        // 通过 Unit 的 `m_next_free_unit_offset` 字段，形成一个头插链表
        // 如果一个页面耗尽，其 `m_next_allocate_unit_offset` 将直接无缝对接
        // `m_freed_unit_offset`，从而实现内存单元的重复利用。
        atomic_uint16_t m_freed_unit_offset;

        // Donot need atomic, allocate unit in page will happend in single thread.
        uint16_t m_next_allocate_unit_offset;
    };
    static_assert(sizeof(PageHeader) == 8, "PageHeader size too large");

    struct UnitHeader
    {
        std::atomic_uint8_t m_allocated_flag;
        woomem_MemoryAttribute m_memory_attribute;

        uint16_t m_next_free_unit_offset;


    };
    

    union Page
    {
        char m_entire_page[MEMORY_PAGE_SIZE];
        struct
        {
            PageHeader m_header;
            char m_page_storage[MEMORY_PAGE_SIZE - sizeof(PageHeader)];
        };
    };
    static_assert(sizeof(Page) == MEMORY_PAGE_SIZE, "Page size mismatch");

    class Chunk
    {
    public:
        Page* const m_chunk_begin_addr;
        Page* const m_chunk_end_addr;

        Chunk* m_next_chunk;

        atomic_size_t m_next_commit_page_index;
    private:
        inline static std::atomic<Chunk*> g_chunks{ nullptr };

    private:
        // Disallow copy and move
        Chunk(const Chunk&) = delete;
        Chunk& operator=(const Chunk&) = delete;
        Chunk(Chunk&&) = delete;
        Chunk& operator=(Chunk&&) = delete;

    private:
        Chunk(void* chunk_storages) noexcept
            : m_chunk_begin_addr(reinterpret_cast<Page*>(chunk_storages))
            , m_chunk_end_addr(reinterpret_cast<Page*>(chunk_storages) + MEMORY_CHUNK_SIZE)
            , m_next_chunk(nullptr)
            , m_next_commit_page_index{ 0 }
        {
            assert(chunk_storages != nullptr);
        }
        ~Chunk()
        {
            // TODO;
        }

        /* OPTIONAL */ Page* try_allocate_new_page() noexcept
        {
            if (m_next_commit_page_index.load(std::memory_order_relaxed) >= MEMORY_PAGES_PER_CHUNK)
                // No more pages available
                return nullptr;

            const size_t page_index = m_next_commit_page_index.fetch_add(1, std::memory_order_relaxed);
            if (page_index >= MEMORY_PAGES_PER_CHUNK)
                // No more pages available
                return nullptr;

            return m_chunk_begin_addr + page_index;
        }

    public:
        static Page* try_allocate_page() noexcept
        {
            do
            {
                for (Chunk* current_chunk = g_chunks.load(std::memory_order_acquire);
                    current_chunk != nullptr;
                    current_chunk = current_chunk->m_next_chunk)
                {
                    Page* new_page = current_chunk->try_allocate_new_page();
                    if (new_page != nullptr)
                    {
                        return new_page;
                    }
                }

                // All chunks are run out of pages, try to allocate a new chunk.
                if (nullptr == try_allocate_new_chunk())
                {
                    // Failed to allocate new chunk.
                    return nullptr;
                }

            } while (true);
        }
        static /* OPTIONAL */ Chunk* try_allocate_new_chunk() noexcept
        {
            void* new_chunk_instance_storage = malloc(sizeof(Chunk));
            if (new_chunk_instance_storage == /* C-API */ NULL)
            {
                // Failed to allocate Chunk structure
                return nullptr;
            }

            void* new_chunk_storages = woomem_os_reserve_memory(MEMORY_CHUNK_SIZE);
            if (new_chunk_storages == /* C-API */ NULL)
            {
                // Failed to reserve memory from OS
                free(new_chunk_instance_storage);
                return nullptr;
            }
            Chunk* new_chunk = new(new_chunk_instance_storage) Chunk(new_chunk_storages);

            Chunk* expected = g_chunks.load(std::memory_order_relaxed);
            do
            {
                new_chunk->m_next_chunk = expected;

            } while (!g_chunks.compare_exchange_weak(
                expected,
                new_chunk,
                std::memory_order_release,
                std::memory_order_relaxed));

            return new_chunk;
        }
        static void release_chunk(Chunk* chunk) noexcept
        {
            void* chunk_storage = chunk->m_chunk_begin_addr;

            chunk->~Chunk();
            free(chunk->m_chunk_begin_addr);

            woomem_os_release_memory(
                chunk_storage,
                MEMORY_CHUNK_SIZE);
        }


    };
}