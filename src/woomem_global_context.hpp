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
        union
        {
            char __keep__;
            struct
            {
                Chunk m_chunk;
                GlobalPageCollection m_global_page_collection;
            };
        };
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

        void add_page_into_chain(PageHead* page);
        PageHead* allocate_huge_page(size_t size);
    };

    extern GlobalContext g_global_context;
}
