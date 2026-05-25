#pragma once

#include "woomem_chunk.hpp"
#include "woomem_global_page_collection.hpp"

#include <atomic>
#include <mutex>
#include <unordered_set>

namespace woomem
{
    class ThreadContext;

    class GlobalContext
    {
    public:
        bool m_globalcontext_alive;
        bool m_globalcontext_inited;

        std::mutex m_thread_entries_mx;
        std::unordered_set<ThreadContext*> m_thread_entries;
        std::atomic<PageHead*> m_all_page_list;

        GlobalContext();
        ~GlobalContext();

        GlobalContext(const GlobalContext&) = delete;
        GlobalContext& operator=(const GlobalContext&) = delete;
        GlobalContext(GlobalContext&&) = delete;
        GlobalContext& operator=(GlobalContext&&) = delete;

        bool init(size_t reserved_chunk_size);
        void shutdown();

        void add_new_page_into_chain(PageHead* page);
        void add_page_back_to_into_chain(PageHead* page);
        PageHead* allocate_huge_page(size_t size);

        Chunk& chunk() { return reinterpret_cast<Chunk&>(m_chunk_storage); }
        const Chunk& chunk() const { return reinterpret_cast<const Chunk&>(m_chunk_storage); }
        GlobalPageCollection& gpc() { return reinterpret_cast<GlobalPageCollection&>(m_gpc_storage); }
        const GlobalPageCollection& gpc() const { return reinterpret_cast<const GlobalPageCollection&>(m_gpc_storage); }

    private:
        alignas(Chunk) char m_chunk_storage[sizeof(Chunk)];
        alignas(GlobalPageCollection) char m_gpc_storage[sizeof(GlobalPageCollection)];
    };

    extern GlobalContext g_global_context;
}
