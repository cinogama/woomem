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

    // 超长元素缓存临界，WooMem 将缓存超长元素的解分配，直到缓存的总容量超过此
    // 限制。对于大小天然超过此限制的超长元素，WooMem 将在其解分配时直接释放。
    constexpr size_t MEMORY_EXUINT_CACHE_SIZE = 8 * 1024 * 1024;

    /*
    WooMem 内存管理逻辑

    内存分配器由一系列的 块(Chunk) 构成，一个块包含若干个 页面(Page)，
    当块完全分配完毕之后，内存分配器将尝试申请一个新的块。每个页面
    包含若干个 内存单元(Memory Unit)，页面有自己的独有划分大小，一旦
    一个页面被划分为某个大小的内存单元后，该页面只能分配该大小的内存单
    元（除非此页面在某轮 GC 之后被完全释放）。

    超长元素：如果分配的单元大于 65520（一个 Page 的最大单元空间），此
    类元素被称为超长元素，超长元素的分配需要使用另一套实现——它们将不再
    按照 Page 管理和分配，参见 MEMORY_EXUINT_CACHE_SIZE。
    */

    struct PageHeader
    {
        uint16_t m_unit_size_in_this_page;
        uint16_t m_total_unit_count_in_this_page;

        // Donot need atomic, allocate unit in page will happend in single thread.
        // 指向 UnitHeader
        uint16_t m_next_allocate_unit_offset;

        // Free operation might happend in multi-threaded environment.
        // `m_freed_unit_offset` 储存刚刚被释放的内存单元的偏移量，
        // 通过 Unit 的 `m_next_free_unit_offset` 字段，形成一个头插链表
        // 如果一个页面耗尽，其 `m_next_allocate_unit_offset` 将直接无缝对接
        // `m_freed_unit_offset`，从而实现内存单元的重复利用。
        atomic_uint16_t m_freed_unit_offset;

    };
    static_assert(sizeof(PageHeader) == MEMORY_UNIT_BASE_ALIGN, "PageHeader size too large");

    constexpr size_t MEMORY_PAGE_REAL_STORAGE_SIZE = MEMORY_PAGE_SIZE - sizeof(PageHeader);

    struct UnitHeader
    {
        // 如果 UnitHeader 被用于描述一个超长单元（不属于 Page) 的单元，单元的前 16 位
        // 需要保持为0，这能让其他需要遍历 Page 的操作认识到这不是一个 Page，而是一个
        // 超长单元。
        uint16_t _reserve_and_keep_zero_if_exlong_unit_;

        // 内存单元的属性信息
        woomem_MemoryAttribute m_memory_attribute;

        // 标记当前内存单元是否已经被分配，0 表示未分配，1 表示已分配
        std::atomic_uint8_t m_allocated_flag;

        // 用于构建空闲单元链表，用于指示下一个可用单元在块内的偏移量（指向 UnitHeader）
        // 如果为 0，表示没有下一个可用单元
        uint16_t m_next_free_unit_offset;

        // 预留，目前仅用以确保单元的实际存储按 MEMORY_UNIT_BASE_ALIGN 对齐
        char _reserved_[2];
    };
    static_assert(sizeof(UnitHeader) == MEMORY_UNIT_BASE_ALIGN, "UnitHeader size too large");
    static_assert(
        offsetof(UnitHeader, _reserve_and_keep_zero_if_exlong_unit_) == offsetof(PageHeader, m_unit_size_in_this_page),
        "UnitHeader and PageHeader layout mismatch");

    union Page
    {
        alignas(MEMORY_UNIT_BASE_ALIGN) char m_entire_page_storage[MEMORY_PAGE_SIZE];
        struct
        {
            PageHeader m_header;
            alignas(MEMORY_UNIT_BASE_ALIGN) char m_page_storage[MEMORY_PAGE_REAL_STORAGE_SIZE];
        };

        // 仅当在当前页面完全空闲时，才能使用此接口初始化页面
        void _init_by_size(uint16_t unit_size) noexcept
        {
            // unit_size must be multiple of MEMORY_UNIT_BASE_ALIGN
            assert(unit_size % MEMORY_UNIT_BASE_ALIGN == 0);

            const size_t unit_and_header_size = unit_size + sizeof(UnitHeader);

            m_header.m_unit_size_in_this_page = unit_size;
            m_header.m_total_unit_count_in_this_page =
                static_cast<uint16_t>(MEMORY_PAGE_REAL_STORAGE_SIZE / unit_and_header_size);

            uint16_t* make_free_chain_place = &m_header.m_next_allocate_unit_offset;
            for (size_t offset = offsetof(Page, m_page_storage);
                offset + unit_and_header_size < MEMORY_PAGE_SIZE;
                offset += unit_and_header_size)
            {
                *make_free_chain_place = static_cast<uint16_t>(offset);

                UnitHeader* unit_header =
                    std::launder(reinterpret_cast<UnitHeader*>(m_entire_page_storage + offset));

                // 初始化每个存储单元的分配状态，属性不需要初始化（后续流程可以确保绝不发生属性在初
                // 始化之前发生的读取操作）。
                unit_header->m_allocated_flag.store(0, std::memory_order_relaxed);

                make_free_chain_place = &unit_header->m_next_free_unit_offset;
            }

            // 对于空闲链表的最后一个单元，其 m_next_allocate_unit_offset 应当为 0，表示没有后继的
            // 空闲元素了
            *make_free_chain_place = 0;

            // 初始化释放链表为空
            m_header.m_freed_unit_offset.store(0, std::memory_order_release);
        }

        Page(const Page&) = delete;
        Page& operator=(const Page&) = delete;
        Page(Page&&) = delete;
        Page& operator=(Page&&) = delete;

    protected:
        Page(uint16_t unit_size)
        {
            _init_by_size(unit_size);
        }
        ~Page() = default;

        friend class Chunk;
    };
    static_assert(sizeof(Page) == MEMORY_PAGE_SIZE, "Page size mismatch");
    static_assert(offsetof(Page, m_header) == 0, "PageHeader offset mismatch");
    static_assert(offsetof(Page, m_entire_page_storage) == 0, "Page storage offset mismatch");
    static_assert(offsetof(Page, m_page_storage) == sizeof(PageHeader), "Page storage offset mismatch");

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

    /*
    GlobalPageCollection 用于保管所有的空闲页面，按照分配单元大小进行分类管理。
    当需要分配某个大小的内存单元时，优先从 ThreadLocalPageCollection 中获取可用页面，
    如果没有，再从 GlobalPageCollection 中获取。
    */
    class GlobalPageCollection
    {
        
    };
    GlobalPageCollection g_global_page_collection;

    /*
    ThreadLocalPageCollection 用于保管线程局域的空闲页面，按照分配单元大小进行分类管理。
    */
    class ThreadLocalPageCollection
    {
    };
}