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

namespace woomem_cppimpl
{
    // ============================================================================
    // 常量定义
    // ============================================================================
    
    // 内存块大小类别（Size Classes）- 使用类似 jemalloc/tcmalloc 的分级策略
    // 小对象: 8, 16, 24, 32, 48, 64, 80, 96, 112, 128, 192, 256, 384, 512, 768, 1024
    // 中对象: 2KB, 4KB, 8KB, 16KB, 32KB
    // 大对象: > 32KB，直接从全局堆分配
    
    constexpr size_t ALIGNMENT = 8;
    constexpr size_t MIN_ALLOC_SIZE = 8;
    constexpr size_t MAX_SMALL_SIZE = 1024;      // 小对象上限
    constexpr size_t MAX_MEDIUM_SIZE = 32 * 1024; // 中对象上限
    
    // 每个 Chunk 的大小（用于批量从OS获取内存）
    constexpr size_t CHUNK_SIZE = 256 * 1024;    // 256KB
    
    // ThreadCache 每个 size class 的最大缓存数量
    constexpr size_t THREAD_CACHE_MAX_SIZE = 64;
    
    // Small size classes
    constexpr size_t SMALL_SIZE_CLASSES[] = {
        8, 16, 24, 32, 48, 64, 80, 96, 112, 128, 192, 256, 384, 512, 768, 1024
    };
    constexpr size_t NUM_SMALL_CLASSES = sizeof(SMALL_SIZE_CLASSES) / sizeof(SMALL_SIZE_CLASSES[0]);
    
    // Medium size classes (2KB - 32KB, 每次翻倍)
    constexpr size_t MEDIUM_SIZE_CLASSES[] = {
        2 * 1024, 4 * 1024, 8 * 1024, 16 * 1024, 32 * 1024
    };
    constexpr size_t NUM_MEDIUM_CLASSES = sizeof(MEDIUM_SIZE_CLASSES) / sizeof(MEDIUM_SIZE_CLASSES[0]);
    
    constexpr size_t TOTAL_SIZE_CLASSES = NUM_SMALL_CLASSES + NUM_MEDIUM_CLASSES;
    
    // ============================================================================
    // 辅助函数
    // ============================================================================
    
    // 对齐到指定边界
    constexpr size_t align_up(size_t size, size_t alignment) {
        return (size + alignment - 1) & ~(alignment - 1);
    }
    
    // 根据请求大小找到对应的 size class index
    inline size_t get_size_class_index(size_t size) {
        // 小对象
        for (size_t i = 0; i < NUM_SMALL_CLASSES; ++i) {
            if (size <= SMALL_SIZE_CLASSES[i]) {
                return i;
            }
        }
        // 中对象
        for (size_t i = 0; i < NUM_MEDIUM_CLASSES; ++i) {
            if (size <= MEDIUM_SIZE_CLASSES[i]) {
                return NUM_SMALL_CLASSES + i;
            }
        }
        // 大对象，返回无效索引
        return TOTAL_SIZE_CLASSES;
    }
    
    // 根据 size class index 获取实际分配大小
    inline size_t get_size_from_class(size_t class_index) {
        if (class_index < NUM_SMALL_CLASSES) {
            return SMALL_SIZE_CLASSES[class_index];
        } else if (class_index < TOTAL_SIZE_CLASSES) {
            return MEDIUM_SIZE_CLASSES[class_index - NUM_SMALL_CLASSES];
        }
        return 0; // 大对象
    }
    
    // ============================================================================
    // 内存单元头部结构
    // ============================================================================
    
    struct BlockHeader {
        // 用户数据大小（不包括头部）
        uint32_t m_user_size;
        
        // Size class index，用于快速定位回收列表
        uint16_t m_size_class;
        
        // 标志位
        uint8_t m_flags;
        
        // GC 属性
        woomem_MemoryAttribute m_gc_attr;
        
        // 空闲链表指针（仅当块空闲时使用，复用用户数据区域）
        // 这里不存储，而是存在用户数据区域
        
        static constexpr uint8_t FLAG_ALLOCATED = 0x01;
        static constexpr uint8_t FLAG_LARGE_BLOCK = 0x02;
        
        bool is_allocated() const { return (m_flags & FLAG_ALLOCATED) != 0; }
        bool is_large_block() const { return (m_flags & FLAG_LARGE_BLOCK) != 0; }
        
        void set_allocated(bool v) {
            if (v) m_flags |= FLAG_ALLOCATED;
            else m_flags &= ~FLAG_ALLOCATED;
        }
        
        void set_large_block(bool v) {
            if (v) m_flags |= FLAG_LARGE_BLOCK;
            else m_flags &= ~FLAG_LARGE_BLOCK;
        }
    };
    
    static_assert(sizeof(BlockHeader) <= 8, "BlockHeader should be compact");
    
    // 头部大小，对齐到 ALIGNMENT
    constexpr size_t HEADER_SIZE = align_up(sizeof(BlockHeader), ALIGNMENT);
    
    // 从用户指针获取头部
    inline BlockHeader* get_header(void* user_ptr) {
        return reinterpret_cast<BlockHeader*>(
            reinterpret_cast<char*>(user_ptr) - HEADER_SIZE
        );
    }
    
    // 从头部获取用户指针
    inline void* get_user_ptr(BlockHeader* header) {
        return reinterpret_cast<char*>(header) + HEADER_SIZE;
    }
    
    // ============================================================================
    // 空闲链表节点（存储在空闲块的用户数据区域）
    // ============================================================================
    
    struct FreeNode {
        FreeNode* m_next;
    };
    
    // ============================================================================
    // Chunk 管理 - 从OS分配的大块内存
    // ============================================================================
    
    struct Chunk {
        Chunk* m_next;
        size_t m_size;
        size_t m_used;
        char m_data[1]; // 柔性数组
        
        char* alloc_raw(size_t size) {
            size = align_up(size, ALIGNMENT);
            if (m_used + size > m_size) {
                return nullptr;
            }
            char* ptr = m_data + m_used;
            m_used += size;
            return ptr;
        }
        
        bool contains(void* ptr) const {
            const char* p = reinterpret_cast<const char*>(ptr);
            return p >= m_data && p < m_data + m_size;
        }
    };
    
    // ============================================================================
    // 大对象节点
    // ============================================================================
    
    struct LargeBlock {
        LargeBlock* m_prev;
        LargeBlock* m_next;
        size_t m_total_size;  // 包括整个分配的大小
        // 后面紧跟 BlockHeader，然后是用户数据
        
        // 获取内嵌的 BlockHeader
        BlockHeader* get_header() {
            return reinterpret_cast<BlockHeader*>(
                reinterpret_cast<char*>(this) + sizeof(LargeBlock)
            );
        }
        
        // 获取用户数据指针
        void* get_user_ptr() {
            return reinterpret_cast<char*>(this) + sizeof(LargeBlock) + HEADER_SIZE;
        }
        
        // 从用户指针获取 LargeBlock
        static LargeBlock* from_user_ptr(void* user_ptr) {
            return reinterpret_cast<LargeBlock*>(
                reinterpret_cast<char*>(user_ptr) - HEADER_SIZE - sizeof(LargeBlock)
            );
        }
    };
    
    // ============================================================================
    // CentralCache - 全局中央缓存
    // ============================================================================
    
    class CentralCache {
    public:
        void init() {
            for (size_t i = 0; i < TOTAL_SIZE_CLASSES; ++i) {
                m_free_lists[i] = nullptr;
                m_free_counts[i] = 0;
            }
            m_chunks = nullptr;
            m_large_blocks = nullptr;
            m_current_gc_timing = 0;
            m_is_full_gc = false;
        }
        
        void shutdown() {
            // 释放所有 chunks
            Chunk* chunk = m_chunks;
            while (chunk) {
                Chunk* next = chunk->m_next;
                size_t total_size = sizeof(Chunk) - 1 + chunk->m_size;
                woomem_os_decommit_memory(chunk, total_size);
                woomem_os_release_memory(chunk, total_size);
                chunk = next;
            }
            m_chunks = nullptr;
            
            // 释放所有大对象
            LargeBlock* lb = m_large_blocks;
            while (lb) {
                LargeBlock* next = lb->m_next;
                woomem_os_decommit_memory(lb, lb->m_total_size);
                woomem_os_release_memory(lb, lb->m_total_size);
                lb = next;
            }
            m_large_blocks = nullptr;
        }
        
        // 从中央缓存获取一批空闲块
        FreeNode* fetch_batch(size_t class_index, size_t& out_count) {
            lock_guard<mutex> lock(m_mutex);
            
            FreeNode* head = m_free_lists[class_index];
            if (!head) {
                // 中央缓存为空，需要从 Chunk 分配新块
                head = allocate_new_blocks(class_index, out_count);
                return head;
            }
            
            // 取出最多 THREAD_CACHE_MAX_SIZE / 2 个块
            size_t max_fetch = THREAD_CACHE_MAX_SIZE / 2;
            size_t count = 0;
            FreeNode* tail = head;
            FreeNode* prev = nullptr;
            
            while (tail && count < max_fetch) {
                prev = tail;
                tail = tail->m_next;
                ++count;
            }
            
            if (prev) {
                prev->m_next = nullptr;
            }
            
            m_free_lists[class_index] = tail;
            m_free_counts[class_index] -= count;
            out_count = count;
            
            return head;
        }
        
        // 归还一批空闲块到中央缓存
        void return_batch(size_t class_index, FreeNode* head, size_t count) {
            if (!head || count == 0) return;
            
            // 找到链表尾部
            FreeNode* tail = head;
            while (tail->m_next) {
                tail = tail->m_next;
            }
            
            lock_guard<mutex> lock(m_mutex);
            tail->m_next = m_free_lists[class_index];
            m_free_lists[class_index] = head;
            m_free_counts[class_index] += count;
        }
        
        // 分配大对象
        void* alloc_large(size_t user_size) {
            size_t page_size = woomem_os_page_size();
            size_t total_size = align_up(sizeof(LargeBlock) + HEADER_SIZE + user_size, page_size);
            
            void* mem = woomem_os_reserve_memory(total_size);
            if (!mem) return nullptr;
            
            if (woomem_os_commit_memory(mem, total_size) != 0) {
                woomem_os_release_memory(mem, total_size);
                return nullptr;
            }
            
            LargeBlock* lb = reinterpret_cast<LargeBlock*>(mem);
            lb->m_total_size = total_size;
            
            BlockHeader* header = lb->get_header();
            header->m_user_size = static_cast<uint32_t>(user_size);
            header->m_size_class = static_cast<uint16_t>(TOTAL_SIZE_CLASSES);
            header->m_flags = BlockHeader::FLAG_ALLOCATED | BlockHeader::FLAG_LARGE_BLOCK;
            header->m_gc_attr.m_gc_age = 15;
            header->m_gc_attr.m_gc_marked = WOOMEM_GC_MARKED_UNMARKED;
            header->m_gc_attr.m_alloc_timing = m_current_gc_timing;
            
            // 链入大对象列表
            {
                lock_guard<mutex> lock(m_mutex);
                lb->m_prev = nullptr;
                lb->m_next = m_large_blocks;
                if (m_large_blocks) {
                    m_large_blocks->m_prev = lb;
                }
                m_large_blocks = lb;
            }
            
            return lb->get_user_ptr();
        }
        
        // 释放大对象
        void free_large(LargeBlock* lb) {
            size_t total_size = lb->m_total_size;
            
            {
                lock_guard<mutex> lock(m_mutex);
                if (lb->m_prev) {
                    lb->m_prev->m_next = lb->m_next;
                } else {
                    m_large_blocks = lb->m_next;
                }
                if (lb->m_next) {
                    lb->m_next->m_prev = lb->m_prev;
                }
            }
            
            woomem_os_decommit_memory(lb, total_size);
            woomem_os_release_memory(lb, total_size);
        }
        
        // GC 相关
        uint8_t get_current_gc_timing() const { return m_current_gc_timing; }
        bool is_full_gc() const { return m_is_full_gc; }
        
        void begin_gc_mark(bool is_full_gc) {
            lock_guard<mutex> lock(m_mutex);
            m_current_gc_timing = (m_current_gc_timing + 1) & 0x03;
            m_is_full_gc = is_full_gc;
        }
        
        // 验证指针是否为有效分配的内存
        BlockHeader* validate_ptr(intptr_t maybe_ptr) {
            if (maybe_ptr == 0) return nullptr;
            
            // 检查对齐
            if ((maybe_ptr & (ALIGNMENT - 1)) != 0) return nullptr;
            
            void* ptr = reinterpret_cast<void*>(maybe_ptr);
            
            lock_guard<mutex> lock(m_mutex);
            
            // 检查是否在某个 chunk 中
            for (Chunk* chunk = m_chunks; chunk; chunk = chunk->m_next) {
                if (chunk->contains(ptr)) {
                    // 计算头部位置
                    BlockHeader* header = get_header(ptr);
                    if (reinterpret_cast<char*>(header) >= chunk->m_data &&
                        header->is_allocated()) {
                        return header;
                    }
                    return nullptr;
                }
            }
            
            // 检查是否是大对象
            for (LargeBlock* lb = m_large_blocks; lb; lb = lb->m_next) {
                void* user_ptr = lb->get_user_ptr();
                BlockHeader* header = lb->get_header();
                if (user_ptr == ptr && header->is_allocated()) {
                    return header;
                }
            }
            
            return nullptr;
        }
        
        // 遍历所有已分配的块执行 GC 回收
        void gc_sweep(woomem_DestroyFunc destroy_func, void* userdata) {
            lock_guard<mutex> lock(m_mutex);
            
            uint8_t current_timing = m_current_gc_timing;
            bool is_full = m_is_full_gc;
            
            // 遍历所有 chunks 中的块
            for (Chunk* chunk = m_chunks; chunk; chunk = chunk->m_next) {
                char* ptr = chunk->m_data;
                char* end = chunk->m_data + chunk->m_used;
                
                while (ptr < end) {
                    BlockHeader* header = reinterpret_cast<BlockHeader*>(ptr);
                    size_t block_size = HEADER_SIZE + get_size_from_class(header->m_size_class);
                    
                    if (header->is_allocated()) {
                        bool should_free = should_gc_free(header, current_timing, is_full);
                        
                        if (should_free) {
                            // 调用析构函数
                            if (destroy_func) {
                                destroy_func(get_user_ptr(header), userdata);
                            }
                            
                            // 释放到空闲链表
                            header->set_allocated(false);
                            FreeNode* node = reinterpret_cast<FreeNode*>(get_user_ptr(header));
                            node->m_next = m_free_lists[header->m_size_class];
                            m_free_lists[header->m_size_class] = node;
                            m_free_counts[header->m_size_class]++;
                        } else {
                            // 更新 GC 属性
                            update_gc_attr_after_survive(header);
                        }
                    }
                    
                    ptr += block_size;
                }
            }
            
            // 处理大对象
            LargeBlock* lb = m_large_blocks;
            while (lb) {
                LargeBlock* next = lb->m_next;
                BlockHeader* header = lb->get_header();
                
                if (header->is_allocated()) {
                    bool should_free = should_gc_free(header, current_timing, is_full);
                    
                    if (should_free) {
                        if (destroy_func) {
                            void* user_ptr = lb->get_user_ptr();
                            destroy_func(user_ptr, userdata);
                        }
                        
                        // 从链表移除
                        if (lb->m_prev) {
                            lb->m_prev->m_next = lb->m_next;
                        } else {
                            m_large_blocks = lb->m_next;
                        }
                        if (lb->m_next) {
                            lb->m_next->m_prev = lb->m_prev;
                        }
                        
                        // 释放内存
                        woomem_os_decommit_memory(lb, lb->m_total_size);
                        woomem_os_release_memory(lb, lb->m_total_size);
                    } else {
                        update_gc_attr_after_survive(header);
                    }
                }
                
                lb = next;
            }
        }
        
    private:
        mutex m_mutex;
        FreeNode* m_free_lists[TOTAL_SIZE_CLASSES];
        size_t m_free_counts[TOTAL_SIZE_CLASSES];
        Chunk* m_chunks;
        LargeBlock* m_large_blocks;
        uint8_t m_current_gc_timing;
        bool m_is_full_gc;
        
        // 从 chunk 分配新块
        FreeNode* allocate_new_blocks(size_t class_index, size_t& out_count) {
            size_t block_size = HEADER_SIZE + get_size_from_class(class_index);
            size_t num_blocks = min(THREAD_CACHE_MAX_SIZE / 2, CHUNK_SIZE / block_size);
            
            // 确保当前 chunk 有足够空间，或者分配新 chunk
            Chunk* chunk = m_chunks;
            if (!chunk || chunk->m_used + block_size * num_blocks > chunk->m_size) {
                chunk = allocate_new_chunk();
                if (!chunk) {
                    out_count = 0;
                    return nullptr;
                }
            }
            
            // 分配块
            FreeNode* head = nullptr;
            FreeNode* tail = nullptr;
            size_t count = 0;
            
            for (size_t i = 0; i < num_blocks; ++i) {
                char* raw = chunk->alloc_raw(block_size);
                if (!raw) break;
                
                BlockHeader* header = reinterpret_cast<BlockHeader*>(raw);
                header->m_user_size = static_cast<uint32_t>(get_size_from_class(class_index));
                header->m_size_class = static_cast<uint16_t>(class_index);
                header->m_flags = 0;
                header->m_gc_attr.m_gc_age = 15;
                header->m_gc_attr.m_gc_marked = WOOMEM_GC_MARKED_UNMARKED;
                header->m_gc_attr.m_alloc_timing = m_current_gc_timing;
                
                FreeNode* node = reinterpret_cast<FreeNode*>(get_user_ptr(header));
                node->m_next = nullptr;
                
                if (!head) {
                    head = tail = node;
                } else {
                    tail->m_next = node;
                    tail = node;
                }
                ++count;
            }
            
            out_count = count;
            return head;
        }
        
        Chunk* allocate_new_chunk() {
            size_t page_size = woomem_os_page_size();
            size_t chunk_alloc_size = align_up(sizeof(Chunk) - 1 + CHUNK_SIZE, page_size);
            
            void* mem = woomem_os_reserve_memory(chunk_alloc_size);
            if (!mem) return nullptr;
            
            if (woomem_os_commit_memory(mem, chunk_alloc_size) != 0) {
                woomem_os_release_memory(mem, chunk_alloc_size);
                return nullptr;
            }
            
            Chunk* chunk = reinterpret_cast<Chunk*>(mem);
            chunk->m_size = CHUNK_SIZE;
            chunk->m_used = 0;
            chunk->m_next = m_chunks;
            m_chunks = chunk;
            
            return chunk;
        }
        
        bool should_gc_free(BlockHeader* header, uint8_t current_timing, bool is_full) {
            auto& attr = header->m_gc_attr;
            
            // DONOT_RELEASE 不释放
            if (attr.m_gc_marked == WOOMEM_GC_MARKED_DONOT_RELEASE) {
                return false;
            }
            
            // 已标记的不释放
            if (attr.m_gc_marked == WOOMEM_GC_MARKED_SELF_MARKED ||
                attr.m_gc_marked == WOOMEM_GC_MARKED_FULL_MARKED) {
                return false;
            }
            
            // 当前轮次分配的新对象不释放
            if (attr.m_alloc_timing == current_timing && attr.m_gc_age == 15) {
                return false;
            }
            
            // MINOR_GC 不释放老年代对象
            if (!is_full && attr.m_gc_age == 0) {
                return false;
            }
            
            return true;
        }
        
        void update_gc_attr_after_survive(BlockHeader* header) {
            auto& attr = header->m_gc_attr;
            
            // 重置标记状态
            if (attr.m_gc_marked != WOOMEM_GC_MARKED_DONOT_RELEASE) {
                attr.m_gc_marked = WOOMEM_GC_MARKED_UNMARKED;
            }
            
            // 降低年龄
            if (attr.m_gc_age > 0) {
                attr.m_gc_age--;
            }
        }
    };
    
    // ============================================================================
    // ThreadCache - 线程本地缓存
    // ============================================================================
    
    class ThreadCache {
    public:
        void init(CentralCache* central) {
            m_central = central;
            for (size_t i = 0; i < TOTAL_SIZE_CLASSES; ++i) {
                m_free_lists[i] = nullptr;
                m_free_counts[i] = 0;
            }
        }
        
        void shutdown() {
            // 归还所有缓存到中央缓存
            for (size_t i = 0; i < TOTAL_SIZE_CLASSES; ++i) {
                if (m_free_lists[i]) {
                    m_central->return_batch(i, m_free_lists[i], m_free_counts[i]);
                    m_free_lists[i] = nullptr;
                    m_free_counts[i] = 0;
                }
            }
        }
        
        void* alloc(size_t size) {
            size_t class_index = get_size_class_index(size);
            
            // 大对象直接从中央缓存分配
            if (class_index >= TOTAL_SIZE_CLASSES) {
                return m_central->alloc_large(size);
            }
            
            // 尝试从线程本地缓存分配
            FreeNode* node = m_free_lists[class_index];
            if (node) {
                m_free_lists[class_index] = node->m_next;
                m_free_counts[class_index]--;
                
                // 初始化头部
                BlockHeader* header = get_header(node);
                header->set_allocated(true);
                header->m_user_size = static_cast<uint32_t>(size);
                header->m_gc_attr.m_gc_age = 15;
                header->m_gc_attr.m_gc_marked = WOOMEM_GC_MARKED_UNMARKED;
                header->m_gc_attr.m_alloc_timing = m_central->get_current_gc_timing();
                
                return node;
            }
            
            // 从中央缓存获取一批
            size_t count = 0;
            node = m_central->fetch_batch(class_index, count);
            if (!node) {
                return nullptr;
            }
            
            // 取出第一个，其余放入线程缓存
            m_free_lists[class_index] = node->m_next;
            m_free_counts[class_index] = count - 1;
            
            // 初始化头部
            BlockHeader* header = get_header(node);
            header->set_allocated(true);
            header->m_user_size = static_cast<uint32_t>(size);
            header->m_gc_attr.m_gc_age = 15;
            header->m_gc_attr.m_gc_marked = WOOMEM_GC_MARKED_UNMARKED;
            header->m_gc_attr.m_alloc_timing = m_central->get_current_gc_timing();
            
            return node;
        }
        
        void free(void* ptr) {
            BlockHeader* header = get_header(ptr);
            
            // 大对象直接归还中央缓存
            if (header->is_large_block()) {
                LargeBlock* lb = LargeBlock::from_user_ptr(ptr);
                m_central->free_large(lb);
                return;
            }
            
            size_t class_index = header->m_size_class;
            header->set_allocated(false);
            
            // 放入线程本地缓存
            FreeNode* node = reinterpret_cast<FreeNode*>(ptr);
            node->m_next = m_free_lists[class_index];
            m_free_lists[class_index] = node;
            m_free_counts[class_index]++;
            
            // 如果缓存过多，归还一部分到中央缓存
            if (m_free_counts[class_index] > THREAD_CACHE_MAX_SIZE) {
                return_to_central(class_index);
            }
        }
        
    private:
        CentralCache* m_central;
        FreeNode* m_free_lists[TOTAL_SIZE_CLASSES];
        size_t m_free_counts[TOTAL_SIZE_CLASSES];
        
        void return_to_central(size_t class_index) {
            // 归还一半
            size_t return_count = m_free_counts[class_index] / 2;
            FreeNode* head = m_free_lists[class_index];
            FreeNode* prev = nullptr;
            FreeNode* curr = head;
            
            for (size_t i = 0; i < return_count && curr; ++i) {
                prev = curr;
                curr = curr->m_next;
            }
            
            if (prev) {
                prev->m_next = nullptr;
            }
            
            m_central->return_batch(class_index, head, return_count);
            m_free_lists[class_index] = curr;
            m_free_counts[class_index] -= return_count;
        }
    };
    
    // ============================================================================
    // 全局状态
    // ============================================================================
    
    static CentralCache g_central_cache;
    static thread_local ThreadCache* t_thread_cache = nullptr;
    static mutex g_thread_cache_list_mutex;
    static vector<ThreadCache*> g_thread_cache_list;
    static atomic<bool> g_initialized{false};
    
    ThreadCache* get_thread_cache() {
        if (!t_thread_cache) {
            t_thread_cache = new ThreadCache();
            t_thread_cache->init(&g_central_cache);
            
            lock_guard<mutex> lock(g_thread_cache_list_mutex);
            g_thread_cache_list.push_back(t_thread_cache);
        }
        return t_thread_cache;
    }
    
    void cleanup_thread_cache() {
        if (t_thread_cache) {
            t_thread_cache->shutdown();
            
            lock_guard<mutex> lock(g_thread_cache_list_mutex);
            auto it = find(g_thread_cache_list.begin(), g_thread_cache_list.end(), t_thread_cache);
            if (it != g_thread_cache_list.end()) {
                g_thread_cache_list.erase(it);
            }
            
            delete t_thread_cache;
            t_thread_cache = nullptr;
        }
    }
    
} // namespace woomem_cppimpl

// ============================================================================
// C API 实现
// ============================================================================

extern "C" {

void woomem_init(void) {
    using namespace woomem_cppimpl;
    
    if (g_initialized.exchange(true)) {
        return; // 已经初始化
    }
    
    g_central_cache.init();
}

void woomem_shutdown(void) {
    using namespace woomem_cppimpl;
    
    if (!g_initialized.exchange(false)) {
        return; // 未初始化或已关闭
    }
    
    // 清理所有线程缓存
    {
        lock_guard<mutex> lock(g_thread_cache_list_mutex);
        for (auto* tc : g_thread_cache_list) {
            tc->shutdown();
            delete tc;
        }
        g_thread_cache_list.clear();
    }
    t_thread_cache = nullptr;
    
    g_central_cache.shutdown();
}

void* woomem_alloc(size_t size) {
    using namespace woomem_cppimpl;
    
    if (!g_initialized.load()) {
        return nullptr;
    }
    
    if (size == 0) {
        size = 1; // 最小分配 1 字节
    }
    
    return get_thread_cache()->alloc(size);
}

void woomem_free(void* ptr) {
    using namespace woomem_cppimpl;
    
    if (!ptr || !g_initialized.load()) {
        return;
    }
    
    get_thread_cache()->free(ptr);
}

void* woomem_try_mark_self(intptr_t maybe_ptr) {
    using namespace woomem_cppimpl;
    
    if (!g_initialized.load()) {
        return nullptr;
    }
    
    BlockHeader* header = g_central_cache.validate_ptr(maybe_ptr);
    if (!header) {
        return nullptr;
    }
    
    auto& attr = header->m_gc_attr;
    
    // DONOT_RELEASE 不处理
    if (attr.m_gc_marked == WOOMEM_GC_MARKED_DONOT_RELEASE) {
        return nullptr;
    }
    
    // 已经被标记
    if (attr.m_gc_marked == WOOMEM_GC_MARKED_SELF_MARKED ||
        attr.m_gc_marked == WOOMEM_GC_MARKED_FULL_MARKED) {
        return nullptr;
    }
    
    // MINOR_GC 且是老年代
    if (!g_central_cache.is_full_gc() && attr.m_gc_age == 0) {
        return nullptr;
    }
    
    // 标记并返回
    attr.m_gc_marked = WOOMEM_GC_MARKED_SELF_MARKED;
    return reinterpret_cast<void*>(maybe_ptr);
}

void woomem_full_mark(void* ptr) {
    using namespace woomem_cppimpl;
    
    if (!ptr || !g_initialized.load()) {
        return;
    }
    
    BlockHeader* header = get_header(ptr);
    header->m_gc_attr.m_gc_marked = WOOMEM_GC_MARKED_FULL_MARKED;
}

void woomem_begin_gc_mark(woomem_Bool is_full_gc) {
    using namespace woomem_cppimpl;
    
    if (!g_initialized.load()) {
        return;
    }
    
    // 在 GC 开始前，将所有线程缓存归还到中央缓存
    // 这确保 GC 可以遍历所有已分配的块
    {
        lock_guard<mutex> lock(g_thread_cache_list_mutex);
        for (auto* tc : g_thread_cache_list) {
            tc->shutdown();
            tc->init(&g_central_cache);
        }
    }
    
    g_central_cache.begin_gc_mark(is_full_gc == WOOMEM_BOOL_TRUE);
}

void woomem_end_gc_mark_and_free_all_unmarked(
    woomem_DestroyFunc destroy_func,
    void* userdata)
{
    using namespace woomem_cppimpl;
    
    if (!g_initialized.load()) {
        return;
    }
    
    g_central_cache.gc_sweep(destroy_func, userdata);
}

} // extern "C"