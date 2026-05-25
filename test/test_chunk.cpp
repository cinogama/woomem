#include "woomem.h"
#include "woomem_chunk.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
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
    test_##name(); \
    std::printf("  OK   %s\n", #name); \
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

TEST(construct_zero_size)
{
    Chunk chunk(0);
}

TEST(construct_small_size)
{
    Chunk chunk(100);
}

TEST(construct_non_power_of_two)
{
    Chunk chunk(100 * 1024);
}

TEST(construct_exact_power_of_two_pages)
{
    Chunk chunk(1024 * 1024);
}

TEST(allocate_single_page)
{
    Chunk chunk(1024 * 1024);
    PageHead* p = chunk.allocate_page();
    CHECK(p != nullptr);
    chunk.free_page(p);
}

TEST(allocate_exhaust_all_pages)
{
    Chunk chunk(64 * 1024);
    PageHead* pages[2];
    for (int i = 0; i < 2; i++)
    {
        pages[i] = chunk.allocate_page();
        CHECK(pages[i] != nullptr);
    }
    CHECK(chunk.allocate_page() == nullptr);
    for (int i = 0; i < 2; i++)
        chunk.free_page(pages[i]);
}

TEST(allocate_reuse_same_page_after_free)
{
    Chunk chunk(1024 * 1024);
    PageHead* p1 = chunk.allocate_page();
    CHECK(p1 != nullptr);
    chunk.free_page(p1);

    PageHead* p2 = chunk.allocate_page();
    CHECK(p2 != nullptr);
    CHECK_EQ(p1, p2);
    chunk.free_page(p2);
}

TEST(allocate_many_pages)
{
    Chunk chunk(1024 * 1024);
    PageHead* pages[32];
    for (int i = 0; i < 32; i++)
    {
        pages[i] = chunk.allocate_page();
        CHECK(pages[i] != nullptr);
    }
    CHECK(chunk.allocate_page() == nullptr);
    for (int i = 0; i < 32; i++)
        chunk.free_page(pages[i]);
}

TEST(allocate_pages_non_overlapping)
{
    Chunk chunk(1024 * 1024);
    PageHead* a = chunk.allocate_page();
    PageHead* b = chunk.allocate_page();
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK_NE(a, b);
    chunk.free_page(a);
    chunk.free_page(b);
}

TEST(huge_page_smaller_than_one_page)
{
    Chunk chunk(1024 * 1024);
    PageHead* p = chunk.allocate_huge_page(100);
    CHECK(p != nullptr);
    chunk.free_page(p);
}

TEST(huge_page_exact_one_page)
{
    Chunk chunk(1024 * 1024);
    PageHead* p = chunk.allocate_huge_page(PageHead::NORMAL_PAGE_SIZE);
    CHECK(p != nullptr);
    chunk.free_page(p);
}

TEST(huge_page_two_pages)
{
    Chunk chunk(1024 * 1024);
    PageHead* p = chunk.allocate_huge_page(2 * PageHead::NORMAL_PAGE_SIZE);
    CHECK(p != nullptr);
    chunk.free_page(p);
}

TEST(huge_page_large_allocation)
{
    Chunk chunk(4 * 1024 * 1024);
    PageHead* p = chunk.allocate_huge_page(2 * 1024 * 1024);
    CHECK(p != nullptr);
    chunk.free_page(p);
}

TEST(huge_page_exceeds_available)
{
    Chunk chunk(64 * 1024);
    CHECK(chunk.allocate_huge_page(128 * 1024) == nullptr);
}

TEST(validate_nullptr_returns_null)
{
    Chunk chunk(1024 * 1024);
    CHECK(chunk.validate(nullptr) == nullptr);
}

TEST(validate_outside_range_returns_null)
{
    Chunk chunk(1024 * 1024);
    int dummy = 42;
    CHECK(chunk.validate(&dummy) == nullptr);
}

TEST(validate_exact_page_start)
{
    Chunk chunk(1024 * 1024);
    PageHead* p = chunk.allocate_page();
    CHECK(p != nullptr);
    CHECK_EQ(chunk.validate(p), p);
    chunk.free_page(p);
}

TEST(validate_interior_of_page)
{
    Chunk chunk(1024 * 1024);
    PageHead* p = chunk.allocate_page();
    CHECK(p != nullptr);
    char* interior = reinterpret_cast<char*>(p) + 100;
    CHECK_EQ(chunk.validate(interior), p);
    chunk.free_page(p);
}

TEST(validate_freed_returns_null)
{
    Chunk chunk(1024 * 1024);
    PageHead* p = chunk.allocate_page();
    CHECK(p != nullptr);
    chunk.free_page(p);
    CHECK(chunk.validate(p) == nullptr);
}

TEST(validate_huge_page_interior)
{
    Chunk chunk(1024 * 1024);
    PageHead* p = chunk.allocate_huge_page(4 * PageHead::NORMAL_PAGE_SIZE);
    CHECK(p != nullptr);
    char* interior = reinterpret_cast<char*>(p) + 3 * PageHead::NORMAL_PAGE_SIZE + 10;
    CHECK_EQ(chunk.validate(interior), p);
    chunk.free_page(p);
}

TEST(buddy_coalesce_all_to_max_order)
{
    Chunk chunk(1024 * 1024);
    PageHead* pages[32];
    for (int i = 0; i < 32; i++)
    {
        pages[i] = chunk.allocate_page();
        CHECK(pages[i] != nullptr);
    }
    for (int i = 0; i < 32; i++)
        chunk.free_page(pages[i]);
    PageHead* huge = chunk.allocate_huge_page(32 * PageHead::NORMAL_PAGE_SIZE);
    CHECK(huge != nullptr);
    chunk.free_page(huge);
}

TEST(buddy_coalesce_to_order_1)
{
    Chunk chunk(1024 * 1024);
    PageHead* p0 = chunk.allocate_page();
    PageHead* p1 = chunk.allocate_page();
    CHECK(p0 != nullptr);
    CHECK(p1 != nullptr);
    chunk.free_page(p0);
    chunk.free_page(p1);
    PageHead* p2 = chunk.allocate_huge_page(2 * PageHead::NORMAL_PAGE_SIZE);
    CHECK(p2 != nullptr);
    chunk.free_page(p2);
}

TEST(buddy_no_coalesce_when_still_allocated)
{
    Chunk chunk(1024 * 1024);
    PageHead* p0 = chunk.allocate_page();
    PageHead* p1 = chunk.allocate_page();
    CHECK(p0 != nullptr);
    CHECK(p1 != nullptr);
    CHECK_EQ(p1, reinterpret_cast<PageHead*>(reinterpret_cast<char*>(p0) + PageHead::NORMAL_PAGE_SIZE));
    chunk.free_page(p1);
    PageHead* p2 = chunk.allocate_huge_page(2 * PageHead::NORMAL_PAGE_SIZE);
    CHECK(p2 != nullptr);
    CHECK_NE(p2, p0);
    chunk.free_page(p0);
    chunk.free_page(p2);
}

TEST(multi_chunk_isolation)
{
    Chunk chunk1(1024 * 1024);
    Chunk chunk2(1024 * 1024);
    PageHead* p1 = chunk1.allocate_page();
    PageHead* p2 = chunk2.allocate_page();
    CHECK(p1 != nullptr);
    CHECK(p2 != nullptr);
    CHECK_NE(p1, p2);
    CHECK_EQ(chunk1.validate(p1), p1);
    CHECK(chunk1.validate(p2) == nullptr);
    CHECK_EQ(chunk2.validate(p2), p2);
    CHECK(chunk2.validate(p1) == nullptr);
    chunk1.free_page(p1);
    chunk2.free_page(p2);
}

TEST(alloc_free_alloc_cycle)
{
    Chunk chunk(1024 * 1024);
    for (int round = 0; round < 10; round++)
    {
        PageHead* a = chunk.allocate_page();
        PageHead* b = chunk.allocate_page();
        PageHead* c = chunk.allocate_page();
        CHECK(a != nullptr);
        CHECK(b != nullptr);
        CHECK(c != nullptr);
        chunk.free_page(a);
        chunk.free_page(b);
        chunk.free_page(c);
    }
}

TEST(huge_page_boundary_case)
{
    Chunk chunk(1024 * 1024);
    size_t sz = PageHead::NORMAL_PAGE_SIZE + 1;
    PageHead* p = chunk.allocate_huge_page(sz);
    CHECK(p != nullptr);
    CHECK_EQ(chunk.validate(p), p);
    chunk.free_page(p);
}

TEST(concurrent_alloc_free_128_pages)
{
    Chunk chunk(4 * 1024 * 1024);

    constexpr int kPages = 128;
    constexpr int kThreads = 4;
    std::atomic<PageHead*> slots[kPages] = {};
    std::atomic<int> next_idx{0};
    std::atomic<int> alloc_count{0};
    std::atomic<int> free_count{0};
    std::atomic<bool> done{false};

    auto alloc_worker = [&]()
    {
        while (true)
        {
            PageHead* p = chunk.allocate_page();
            if (!p)
            {
                if (done.load(std::memory_order_acquire))
                    break;
                continue;
            }
            int slot = next_idx.fetch_add(1, std::memory_order_relaxed);
            if (slot >= kPages)
            {
                chunk.free_page(p);
                done.store(true, std::memory_order_release);
                break;
            }
            slots[slot].store(p, std::memory_order_release);
            alloc_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto free_worker = [&]()
    {
        while (true)
        {
            bool any_freed = false;
            for (int i = 0; i < kPages; i++)
            {
                PageHead* p = slots[i].exchange(nullptr, std::memory_order_acq_rel);
                if (p)
                {
                    CHECK_EQ(chunk.validate(p), p);
                    chunk.free_page(p);
                    free_count.fetch_add(1, std::memory_order_relaxed);
                    any_freed = true;
                }
            }
            if (!any_freed && done.load(std::memory_order_acquire) &&
                alloc_count.load(std::memory_order_relaxed) >= kPages)
                break;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(alloc_worker);
    for (int i = 0; i < 2; i++)
        threads.emplace_back(free_worker);

    for (auto& t : threads)
        t.join();

    CHECK(alloc_count.load() >= kPages);
}

TEST(concurrent_mixed_alloc_free)
{
    Chunk chunk(2 * 1024 * 1024);
    constexpr int kIterations = 500;
    constexpr int kThreads = 4;
    std::atomic<int> ops{0};

    auto worker = [&]()
    {
        for (int i = 0; i < kIterations; i++)
        {
            PageHead* a = chunk.allocate_page();
            if (!a)
                continue;

            PageHead* b = chunk.allocate_page();
            if (!b)
            {
                chunk.free_page(a);
                continue;
            }

            CHECK_NE(a, b);
            CHECK_EQ(chunk.validate(a), a);
            CHECK_EQ(chunk.validate(b), b);

            chunk.free_page(a);
            chunk.free_page(b);
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; i++)
        threads.emplace_back(worker);

    for (auto& t : threads)
        t.join();

    CHECK(ops.load() > 0);
}

TEST(concurrent_huge_page_and_validate)
{
    Chunk chunk(4 * 1024 * 1024);
    std::atomic<bool> stop{false};

    auto alloc_worker = [&]()
    {
        for (int i = 0; i < 200; i++)
        {
            PageHead* p = chunk.allocate_page();
            if (p)
            {
                CHECK_EQ(chunk.validate(p), p);
                chunk.free_page(p);
            }
            else
            {
                break;
            }
        }
    };

    auto huge_worker = [&]()
    {
        for (int i = 0; i < 50; i++)
        {
            PageHead* p = chunk.allocate_huge_page(2 * PageHead::NORMAL_PAGE_SIZE);
            if (p)
            {
                void* interior = reinterpret_cast<char*>(p) + PageHead::NORMAL_PAGE_SIZE + 100;
                CHECK_EQ(chunk.validate(interior), p);
                chunk.free_page(p);
            }
            else
            {
                break;
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 3; i++)
        threads.emplace_back(alloc_worker);
    threads.emplace_back(huge_worker);

    for (auto& t : threads)
        t.join();
}

int test_chunk_main(void)
{
#ifdef _MSC_VER
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    std::printf("=== Chunk Tests ===\n\n");

    RUN_TEST(construct_zero_size);
    RUN_TEST(construct_small_size);
    RUN_TEST(construct_non_power_of_two);
    RUN_TEST(construct_exact_power_of_two_pages);
    RUN_TEST(allocate_single_page);
    RUN_TEST(allocate_exhaust_all_pages);
    RUN_TEST(allocate_reuse_same_page_after_free);
    RUN_TEST(allocate_many_pages);
    RUN_TEST(allocate_pages_non_overlapping);
    RUN_TEST(huge_page_smaller_than_one_page);
    RUN_TEST(huge_page_exact_one_page);
    RUN_TEST(huge_page_two_pages);
    RUN_TEST(huge_page_large_allocation);
    RUN_TEST(huge_page_exceeds_available);
    RUN_TEST(validate_nullptr_returns_null);
    RUN_TEST(validate_outside_range_returns_null);
    RUN_TEST(validate_exact_page_start);
    RUN_TEST(validate_interior_of_page);
    RUN_TEST(validate_freed_returns_null);
    RUN_TEST(validate_huge_page_interior);
    RUN_TEST(buddy_coalesce_all_to_max_order);
    RUN_TEST(buddy_coalesce_to_order_1);
    RUN_TEST(buddy_no_coalesce_when_still_allocated);
    RUN_TEST(multi_chunk_isolation);
    RUN_TEST(alloc_free_alloc_cycle);
    RUN_TEST(huge_page_boundary_case);
    RUN_TEST(concurrent_alloc_free_128_pages);
    RUN_TEST(concurrent_mixed_alloc_free);
    RUN_TEST(concurrent_huge_page_and_validate);

    std::printf("\n=== %d failures ===\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
