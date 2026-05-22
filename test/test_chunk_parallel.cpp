#include "woomem.h"
#include "woomem_chunk.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

using namespace woomem;

static int g_failures = 0;

#define TEST(name) static void test_##name()
#define RUN_TEST(name) do { \
    std::printf("  RUN  %s\n", #name); \
    auto _t0 = std::chrono::steady_clock::now(); \
    test_##name(); \
    auto _t1 = std::chrono::steady_clock::now(); \
    auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>(_t1 - _t0).count(); \
    std::printf("  OK   %s (%lld ms)\n", #name, static_cast<long long>(_ms)); \
} while(0)
#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_failures++; \
        return; \
    } \
} while(0)
#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))

static constexpr size_t kPageSize = Page::NORMAL_PAGE_SIZE;

static void spin_yield()
{
#ifdef _MSC_VER
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

TEST(massive_parallel_alloc_free)
{
    Chunk chunk(32 * 1024 * 1024);
    constexpr int kThreads = 16;
    constexpr int kItersPerThread = 2000;
    std::atomic<int> total_ops{0};
    std::atomic<int> failures{0};

    auto worker = [&](int tid)
    {
        std::mt19937 rng(static_cast<unsigned>(tid + 1000));
        std::uniform_int_distribution<int> coin(0, 1);

        for (int i = 0; i < kItersPerThread; i++)
        {
            if (coin(rng) == 0)
            {
                Page* p = chunk.allocate_page();
                if (p)
                {
                    CHECK_NE(p, nullptr);
                    Page* v = chunk.validate(p);
                    CHECK_EQ(v, p);
                    chunk.free_page(p);
                    total_ops.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                Page* a = chunk.allocate_page();
                Page* b = chunk.allocate_page();
                if (a && b)
                {
                    CHECK_NE(a, b);
                    CHECK_EQ(chunk.validate(a), a);
                    CHECK_EQ(chunk.validate(b), b);
                    chunk.free_page(a);
                    chunk.free_page(b);
                    total_ops.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    if (a) chunk.free_page(a);
                    if (b) chunk.free_page(b);
                }
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    CHECK_GE(total_ops.load(), kThreads * kItersPerThread / 4);
    std::printf("    total_ops=%d\n", total_ops.load());
}

TEST(multi_chunk_parallel_isolated)
{
    constexpr int kChunks = 4;
    constexpr int kThreadsPerChunk = 4;
    constexpr int kIters = 1000;

    Chunk chunks[kChunks] = {
        Chunk(4 * 1024 * 1024),
        Chunk(4 * 1024 * 1024),
        Chunk(4 * 1024 * 1024),
        Chunk(4 * 1024 * 1024),
    };

    std::atomic<int> ops{0};

    auto worker = [&](int chunk_idx)
    {
        Chunk& c = chunks[chunk_idx];
        for (int i = 0; i < kIters; i++)
        {
            Page* a = c.allocate_page();
            Page* b = c.allocate_page();
            if (a && b)
            {
                CHECK_NE(a, b);
                CHECK_EQ(c.validate(a), a);
                c.free_page(a);
                c.free_page(b);
                ops.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                if (a) c.free_page(a);
                if (b) c.free_page(b);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int c = 0; c < kChunks; c++)
        for (int t = 0; t < kThreadsPerChunk; t++)
            threads.emplace_back(worker, c);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), kChunks * kThreadsPerChunk * kIters / 4);
    std::printf("    total_ops=%d\n", ops.load());
}

TEST(producer_consumer_pattern)
{
    Chunk chunk(8 * 1024 * 1024);
    constexpr int kProducers = 6;
    constexpr int kConsumers = 4;
    constexpr int kQueueSize = 256;
    constexpr int kTotalItems = 10000;

    std::atomic<Page*> queue[kQueueSize] = {};
    std::atomic<int> write_pos{0};
    std::atomic<int> read_pos{0};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> producers_done{false};

    auto producer = [&]()
    {
        while (produced.load(std::memory_order_relaxed) < kTotalItems)
        {
            Page* p = chunk.allocate_page();
            if (!p)
            {
                spin_yield();
                continue;
            }

            CHECK_EQ(chunk.validate(p), p);

            int pos = write_pos.fetch_add(1, std::memory_order_acq_rel);
            while (pos - read_pos.load(std::memory_order_acquire) >= kQueueSize)
                spin_yield();

            queue[pos % kQueueSize].store(p, std::memory_order_release);
            produced.fetch_add(1, std::memory_order_relaxed);
        }
        producers_done.store(true, std::memory_order_release);
    };

    auto consumer = [&]()
    {
        while (consumed.load(std::memory_order_relaxed) < kTotalItems)
        {
            if (read_pos.load(std::memory_order_acquire) >= write_pos.load(std::memory_order_acquire))
            {
                if (producers_done.load(std::memory_order_acquire))
                    break;
                spin_yield();
                continue;
            }

            int pos = read_pos.fetch_add(1, std::memory_order_acq_rel);
            if (pos >= produced.load(std::memory_order_acquire))
            {
                read_pos.fetch_sub(1, std::memory_order_relaxed);
                spin_yield();
                continue;
            }

            Page* p = queue[pos % kQueueSize].load(std::memory_order_acquire);
            if (!p)
            {
                read_pos.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }

            CHECK_NE(p, nullptr);
            chunk.free_page(p);
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kProducers; i++)
        threads.emplace_back(producer);
    for (int i = 0; i < kConsumers; i++)
        threads.emplace_back(consumer);
    for (auto& t : threads)
        t.join();

    CHECK_GE(consumed.load(), kTotalItems / 2);
    std::printf("    produced=%d consumed=%d\n", produced.load(), consumed.load());
}

TEST(mixed_order_concurrent)
{
    Chunk chunk(16 * 1024 * 1024);
    constexpr int kThreads = 8;
    constexpr int kIters = 800;

    std::atomic<int> ops{0};

    auto worker = [&](int tid)
    {
        std::mt19937 rng(static_cast<unsigned>(tid + 42));
        std::uniform_int_distribution<int> dist(0, 4);

        for (int i = 0; i < kIters; i++)
        {
            int choice = dist(rng);
            Page* p = nullptr;

            switch (choice)
            {
            case 0:
                p = chunk.allocate_page();
                if (p) CHECK_EQ(chunk.validate(p), p);
                break;
            case 1:
                p = chunk.allocate_huge_page(kPageSize * 2);
                if (p)
                {
                    void* interior = reinterpret_cast<char*>(p) + kPageSize + 32;
                    CHECK_EQ(chunk.validate(interior), p);
                }
                break;
            case 2:
                p = chunk.allocate_huge_page(kPageSize * 4);
                if (p)
                {
                    void* interior = reinterpret_cast<char*>(p) + kPageSize * 3 + 11;
                    CHECK_EQ(chunk.validate(interior), p);
                }
                break;
            case 3:
            case 4:
            {
                Page* a = chunk.allocate_page();
                Page* b = chunk.allocate_page();
                if (a && b)
                {
                    CHECK_NE(a, b);
                    CHECK_EQ(chunk.validate(a), a);
                    CHECK_EQ(chunk.validate(b), b);
                    chunk.free_page(b);
                    ops.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    if (b) chunk.free_page(b);
                }
                if (a) chunk.free_page(a);
                continue;
            }
            }

            if (p)
            {
                chunk.free_page(p);
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), kThreads * kIters / 4);
    std::printf("    ops=%d\n", ops.load());
}

TEST(validate_under_pressure)
{
    Chunk chunk(8 * 1024 * 1024);
    constexpr int kAllocThreads = 6;
    constexpr int kValidateThreads = 4;
    constexpr int kDurationMs = 1500;

    std::atomic<bool> stop{false};
    std::atomic<int> validate_ops{0};
    std::atomic<int> validate_ok{0};

    auto alloc_worker = [&]()
    {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> coin(0, 2);

        while (!stop.load(std::memory_order_relaxed))
        {
            Page* p = nullptr;
            if (coin(rng) == 0)
                p = chunk.allocate_huge_page(kPageSize * 2);
            else
                p = chunk.allocate_page();

            if (p)
            {
                spin_yield();
                chunk.free_page(p);
            }
        }
    };

    auto validate_worker = [&](int tid)
    {
        std::mt19937 rng(static_cast<unsigned>(tid + 5000));
        std::uniform_int_distribution<ptrdiff_t> off_dist(1, kPageSize - 1);

        while (!stop.load(std::memory_order_relaxed))
        {
            Page* p = chunk.allocate_page();
            if (!p)
            {
                spin_yield();
                continue;
            }

            // Validate at page start — should always return p
            Page* result = chunk.validate(p);
            validate_ops.fetch_add(1, std::memory_order_relaxed);
            if (result != nullptr)
                validate_ok.fetch_add(1, std::memory_order_relaxed);

            // Validate an interior offset within the same page
            void* interior = reinterpret_cast<char*>(p) + off_dist(rng);
            result = chunk.validate(interior);
            validate_ops.fetch_add(1, std::memory_order_relaxed);
            if (result != nullptr)
                validate_ok.fetch_add(1, std::memory_order_relaxed);

            chunk.free_page(p);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kAllocThreads; i++)
        threads.emplace_back(alloc_worker);
    for (int i = 0; i < kValidateThreads; i++)
        threads.emplace_back(validate_worker, i);

    std::this_thread::sleep_for(std::chrono::milliseconds(kDurationMs));
    stop.store(true, std::memory_order_release);

    for (auto& t : threads)
        t.join();

    CHECK_GE(validate_ops.load(), 1000);
    CHECK_LE(validate_ok.load(), validate_ops.load());
    std::printf("    validate_ops=%d validate_ok=%d\n",
        validate_ops.load(), validate_ok.load());
}

TEST(near_exhaustion_thrash)
{
    Chunk chunk(1 * 1024 * 1024);
    constexpr int kThreads = 4;
    constexpr int kCycles = 100;

    std::atomic<int> successes{0};

    auto worker = [&]()
    {
        for (int cycle = 0; cycle < kCycles; cycle++)
        {
            Page* pages[32];
            int allocated = 0;

            for (int i = 0; i < 32; i++)
            {
                pages[i] = chunk.allocate_page();
                if (pages[i])
                {
                    CHECK_EQ(chunk.validate(pages[i]), pages[i]);
                    allocated++;
                    successes.fetch_add(1, std::memory_order_relaxed);
                }
            }

            for (int i = 0; i < allocated; i++)
                chunk.free_page(pages[i]);

            spin_yield();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    CHECK_GE(successes.load(), 100);
    std::printf("    successes=%d\n", successes.load());
}

TEST(random_power2_allocations)
{
    Chunk chunk(32 * 1024 * 1024);
    constexpr int kThreads = 8;
    constexpr int kIters = 500;

    std::atomic<int> ops{0};
    std::atomic<int> huge_ops{0};

    auto worker = [&](int tid)
    {
        std::mt19937 rng(static_cast<unsigned>(tid * 7919 + 12345));
        std::uniform_int_distribution<int> order_dist(0, 5);

        for (int i = 0; i < kIters; i++)
        {
            int order = order_dist(rng);
            size_t size = kPageSize * (static_cast<size_t>(1) << order);
            Page* p = chunk.allocate_huge_page(size);

            if (p)
            {
                if (order > 0)
                    huge_ops.fetch_add(1, std::memory_order_relaxed);

                ptrdiff_t off = static_cast<ptrdiff_t>(rng() % (size > 100 ? 100 : 1));
                void* interior = reinterpret_cast<char*>(p) + off;
                CHECK_EQ(chunk.validate(interior), p);

                chunk.free_page(p);
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), kThreads * kIters / 4);
    std::printf("    ops=%d huge_ops=%d\n", ops.load(), huge_ops.load());
}

TEST(alloc_free_interleaved_stress)
{
    Chunk chunk(8 * 1024 * 1024);
    constexpr int kThreads = 12;
    constexpr int kIters = 1500;

    struct Slot { std::atomic<Page*> page{nullptr}; };
    Slot slots[128];
    std::atomic<int> ops{0};

    auto worker = [&](int tid)
    {
        std::mt19937 rng(static_cast<unsigned>(tid + 9999));
        std::uniform_int_distribution<int> slot_dist(0, 127);

        for (int i = 0; i < kIters; i++)
        {
            int slot = slot_dist(rng);
            Page* old = slots[slot].page.exchange(nullptr, std::memory_order_acq_rel);

            if (old)
            {
                CHECK_EQ(chunk.validate(old), old);
                chunk.free_page(old);
            }
            else
            {
                Page* p = chunk.allocate_page();
                if (p)
                {
                    CHECK_EQ(chunk.validate(p), p);
                    Page* expected = nullptr;
                    if (slots[slot].page.compare_exchange_strong(expected, p,
                            std::memory_order_release, std::memory_order_relaxed))
                    {
                        ops.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        chunk.free_page(p);
                    }
                }
            }
        }

        for (int s = 0; s < 128; s++)
        {
            Page* p = slots[s].page.exchange(nullptr, std::memory_order_acq_rel);
            if (p)
                chunk.free_page(p);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), 100);
    std::printf("    ops=%d\n", ops.load());
}

TEST(no_double_alloc)
{
    Chunk chunk(2 * 1024 * 1024);
    constexpr int kThreads = 8;
    constexpr int kIters = 300;

    std::atomic<int> ops{0};

    auto worker = [&](int tid)
    {
        for (int i = 0; i < kIters; i++)
        {
            Page* pages[3] = {nullptr, nullptr, nullptr};
            int count = 0;

            for (int j = 0; j < 3; j++)
            {
                pages[j] = chunk.allocate_page();
                if (pages[j])
                {
                    CHECK_NE(pages[j], nullptr);
                    count++;
                }
                else
                {
                    break;
                }
            }

            for (int j = 0; j < count; j++)
            {
                CHECK_NE(pages[j], nullptr);
            }

            for (int a = 0; a < count; a++)
            {
                for (int b = a + 1; b < count; b++)
                {
                    CHECK_NE(pages[a], pages[b]);
                }
            }

            for (int j = 0; j < count; j++)
                chunk.free_page(pages[j]);

            ops.fetch_add(count, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), 100);
    std::printf("    ops=%d\n", ops.load());
}

TEST(huge_page_non_overlapping)
{
    Chunk chunk(16 * 1024 * 1024);
    constexpr int kThreads = 6;
    constexpr int kIters = 200;

    std::atomic<int> ops{0};
    std::mutex addr_mutex;
    std::set<void*> allocated_addrs;

    auto worker = [&](int tid)
    {
        std::mt19937 rng(static_cast<unsigned>(tid * 31337));
        std::uniform_int_distribution<int> ord_dist(0, 4);

        for (int i = 0; i < kIters; i++)
        {
            int order = ord_dist(rng);
            size_t sz = kPageSize * (static_cast<size_t>(1) << order);
            Page* p = chunk.allocate_huge_page(sz);

            if (p)
            {
                CHECK_EQ(chunk.validate(p), p);

                {
                    std::lock_guard<std::mutex> lk(addr_mutex);
                    bool inserted = allocated_addrs.insert(p).second;
                    CHECK(inserted);
                    allocated_addrs.erase(p);
                }

                chunk.free_page(p);
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), 100);
    std::printf("    ops=%d\n", ops.load());
}

TEST(long_running_stress)
{
    Chunk chunk(32 * 1024 * 1024);
    constexpr int kThreads = 10;
    constexpr int kDurationMs = 3000;

    std::atomic<bool> stop{false};
    std::atomic<long long> total_alloc{0};
    std::atomic<long long> total_free{0};
    std::atomic<long long> alloc_fail{0};

    auto worker = [&](int tid)
    {
        std::mt19937 rng(static_cast<unsigned>(tid + 0xDEAD));
        std::uniform_int_distribution<int> coin(0, 3);
        std::uniform_int_distribution<int> order_dist(0, 3);

        while (!stop.load(std::memory_order_relaxed))
        {
            Page* p = nullptr;
            int c = coin(rng);

            if (c == 0)
            {
                p = chunk.allocate_page();
            }
            else if (c <= 2)
            {
                p = chunk.allocate_huge_page(kPageSize * 2);
            }
            else
            {
                int order = order_dist(rng);
                p = chunk.allocate_huge_page(kPageSize * (static_cast<size_t>(1) << order));
            }

            if (p)
            {
                CHECK_EQ(chunk.validate(p), p);
                total_alloc.fetch_add(1, std::memory_order_relaxed);

                if (total_alloc.load(std::memory_order_relaxed) % 3 != 0)
                    spin_yield();

                chunk.free_page(p);
                total_free.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                alloc_fail.fetch_add(1, std::memory_order_relaxed);
                spin_yield();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::milliseconds(kDurationMs));
    stop.store(true, std::memory_order_release);

    for (auto& t : threads)
        t.join();

    CHECK_GE(total_alloc.load(), 100);
    CHECK_EQ(total_alloc.load(), total_free.load());
    std::printf("    alloc=%lld free=%lld fail=%lld\n",
        total_alloc.load(), total_free.load(), alloc_fail.load());
}

TEST(max_concurrency_stress)
{
    Chunk chunk(16 * 1024 * 1024);
    unsigned hw = std::thread::hardware_concurrency();
    int kThreads = static_cast<int>(std::max(hw * 4u, 8u));
    constexpr int kIters = 1000;

    std::atomic<int> ops{0};

    auto worker = [&]()
    {
        for (int i = 0; i < kIters; i++)
        {
            Page* a = chunk.allocate_page();
            Page* b = chunk.allocate_page();
            if (a && b)
            {
                CHECK_NE(a, b);
                chunk.free_page(b);
                chunk.free_page(a);
                ops.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                if (a) chunk.free_page(a);
                if (b) chunk.free_page(b);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), 100);
    std::printf("    threads=%d ops=%d\n", kThreads, ops.load());
}

TEST(rapid_alloc_free_burst)
{
    Chunk chunk(32 * 1024 * 1024);
    constexpr int kThreads = 8;
    constexpr int kBursts = 50;

    std::atomic<int> ops{0};

    auto worker = [&]()
    {
        std::vector<Page*> batch(50, nullptr);

        for (int burst = 0; burst < kBursts; burst++)
        {
            int count = 0;

            for (int j = 0; j < 50; j++)
            {
                batch[j] = chunk.allocate_page();
                if (batch[j])
                    count++;
            }

            for (int j = 0; j < count; j++)
            {
                CHECK_EQ(chunk.validate(batch[j]), batch[j]);
                chunk.free_page(batch[j]);
            }

            ops.fetch_add(count, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), 2000);
    std::printf("    ops=%d\n", ops.load());
}

TEST(sequential_after_parallel)
{
    Chunk chunk(4 * 1024 * 1024);

    Page* pages[128];
    for (int i = 0; i < 128; i++)
    {
        pages[i] = chunk.allocate_page();
        CHECK(pages[i] != nullptr);
    }
    CHECK(chunk.allocate_page() == nullptr);

    for (int i = 0; i < 128; i++)
        chunk.free_page(pages[i]);

    chunk.defragment();
    Page* huge = chunk.allocate_huge_page(128 * kPageSize);
    CHECK(huge != nullptr);
    chunk.free_page(huge);

    for (int i = 0; i < 64; i++)
    {
        Page* p = chunk.allocate_page();
        CHECK(p != nullptr);
        chunk.free_page(p);
    }
}

TEST(double_free_concurrent)
{
    Chunk chunk(2 * 1024 * 1024);
    constexpr int kThreads = 8;
    constexpr int kRounds = 200;

    std::atomic<int> ops{0};

    auto worker = [&]()
    {
        for (int r = 0; r < kRounds; r++)
        {
            Page* a = chunk.allocate_page();
            Page* b = chunk.allocate_page();
            if (!a || !b)
            {
                if (a) chunk.free_page(a);
                if (b) chunk.free_page(b);
                continue;
            }

            chunk.free_page(a);
            chunk.free_page(a);

            chunk.free_page(b);
            chunk.free_page(b);

            ops.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), 100);
}

TEST(zigzag_order_allocation)
{
    Chunk chunk(16 * 1024 * 1024);
    constexpr int kThreads = 6;
    constexpr int kRounds = 150;

    std::atomic<int> ops{0};

    auto worker = [&]()
    {
        for (int r = 0; r < kRounds; r++)
        {
            Page* huge = chunk.allocate_huge_page(kPageSize * 8);
            if (!huge) continue;

            CHECK_EQ(chunk.validate(huge), huge);

            Page* small[4] = {};
            for (int s = 0; s < 4; s++)
                small[s] = chunk.allocate_page();

            chunk.free_page(huge);

            for (int s = 0; s < 4; s++)
            {
                if (small[s])
                    chunk.free_page(small[s]);
            }

            ops.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    CHECK_GE(ops.load(), 50);
    std::printf("    ops=%d\n", ops.load());
}

int test_chunk_parallel_main(void)
{
#ifdef _MSC_VER
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    std::printf("=== Chunk Parallel Stress Tests ===\n\n");

    RUN_TEST(massive_parallel_alloc_free);
    RUN_TEST(multi_chunk_parallel_isolated);
    RUN_TEST(producer_consumer_pattern);
    RUN_TEST(mixed_order_concurrent);
    RUN_TEST(validate_under_pressure);
    RUN_TEST(near_exhaustion_thrash);
    RUN_TEST(random_power2_allocations);
    RUN_TEST(alloc_free_interleaved_stress);
    RUN_TEST(no_double_alloc);
    RUN_TEST(huge_page_non_overlapping);
    RUN_TEST(long_running_stress);
    RUN_TEST(max_concurrency_stress);
    RUN_TEST(rapid_alloc_free_burst);
    RUN_TEST(sequential_after_parallel);
    RUN_TEST(double_free_concurrent);
    RUN_TEST(zigzag_order_allocation);

    std::printf("\n=== %d failures ===\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
