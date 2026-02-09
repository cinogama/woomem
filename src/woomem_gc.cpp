#include "woomem.h"

#include <vector>
#include <unordered_set>
#include <mutex>

#include "woomem_gc.hpp"

using namespace std;

namespace woomem_cppimpl
{
    struct UnitHead;
}

namespace woomem_cppimpl::gc
{
    GlobalGCMethods g_global_gc_methods;

    struct GrayUnit
    {
        UnitHead* m_unit_head;

        // 在本轮标记结束后，对象是否即将成为老年代对象？
        //  如果是，那么此对象在之后的 FullMark阶段将顺带标记 CardTable
        woomem_Bool m_becoming_old;
    };

    // 如果线程释放，其持有的 CollectorContext 所有权将被临时转移给全局的GC上下文，
    // 并在 GC 结束后统一释放。
    struct CollectorContext
    {
        vector<GrayUnit> m_gray_marked_units;

        CollectorContext() = default;
        ~CollectorContext() = default;

        CollectorContext(const CollectorContext&) = delete;
        CollectorContext(CollectorContext&&) = delete;
        CollectorContext& operator =(const CollectorContext&) = delete;
        CollectorContext& operator =(CollectorContext&&) = delete;
    };

    struct GlobalMarkContext
    {
        mutex m_collector_list_mx;

        unordered_set<CollectorContext*> m_alive_collector_ctxs;
        vector<CollectorContext*> m_droping_collector_ctxs;
    };
    GlobalMarkContext g_global_mark_ctx;

    // ThreadLocalCollector 被用于各个线程自行标记时，临时储存被暂时标
    // 记为灰色的对象实例。
    class ThreadLocalCollector
    {
        CollectorContext* m_context;

        ThreadLocalCollector()
            : m_context(new CollectorContext())
        {
            lock_guard<mutex> g(g_global_mark_ctx.m_collector_list_mx);
            (void)g_global_mark_ctx.m_alive_collector_ctxs.insert(m_context);
        }
        ~ThreadLocalCollector()
        {
            // TODO: If in marking, 

            lock_guard<mutex> g(g_global_mark_ctx.m_collector_list_mx);
            g_global_mark_ctx.m_droping_collector_ctxs.push_back(m_context);
        }

        ThreadLocalCollector(const ThreadLocalCollector&) = delete;
        ThreadLocalCollector(ThreadLocalCollector&&) = delete;
        ThreadLocalCollector& operator =(const ThreadLocalCollector&) = delete;
        ThreadLocalCollector& operator =(ThreadLocalCollector&&) = delete;
    };

    thread_local ThreadLocalCollector t_tls_collector;
}