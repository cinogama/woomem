#include "woomem.h"
#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <cstring>

// 测试统计
struct TestStats {
    int total = 0;
    int passed = 0;
    int failed = 0;
} stats;

// 当前测试名称和错误信息
static const char* current_test_name = nullptr;
static std::string error_message;

// 测试辅助宏
#define TEST(name) \
    do { \
        current_test_name = #name; \
        stats.total++; \
        std::cout << "Running: " << name << "... "; \
        error_message.clear(); \
        if (name()) { \
            stats.passed++; \
            std::cout << "PASSED" << std::endl; \
        } else { \
            stats.failed++; \
            std::cout << "FAILED"; \
            if (!error_message.empty()) { \
                std::cout << ": " << error_message; \
            } \
            std::cout << std::endl; \
        } \
        current_test_name = nullptr; \
    } while(0)

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            error_message = "Assertion failed: " #expr; \
            return false; \
        } \
    } while(0)

#define ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == nullptr) { \
            error_message = "Pointer is null: " #ptr; \
            return false; \
        } \
    } while(0)

#define ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != nullptr) { \
            error_message = "Pointer is not null: " #ptr; \
            return false; \
        } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            char buf[256]; \
            snprintf(buf, sizeof(buf), "%s != %s (got %lld, expected %lld)", #a, #b, \
                     (long long)(a), (long long)(b)); \
            error_message = buf; \
            return false; \
        } \
    } while(0)

// ==================== 基础功能测试 ====================

bool test_basic_allocation() {
    size_t size = 100;
    void* ptr = woomem_alloc_normal(size);
    ASSERT_NOT_NULL(ptr);

    // 写入并验证内存
    memset(ptr, 0xAB, size);
    unsigned char* bytes = (unsigned char*)ptr;
    ASSERT_EQ(bytes[0], 0xAB);
    ASSERT_EQ(bytes[size - 1], 0xAB);

    woomem_free(ptr);
    return true;
}

bool test_basic_realloc() {
    size_t initial_size = 100;
    void* ptr = woomem_alloc_normal(initial_size);
    ASSERT_NOT_NULL(ptr);

    // 写入数据
    memset(ptr, 0xCD, initial_size);

    // 扩大内存
    size_t new_size = 200;
    void* new_ptr = woomem_realloc(ptr, new_size);
    ASSERT_NOT_NULL(new_ptr);

    // 验证原有数据仍然存在
    unsigned char* bytes = (unsigned char*)new_ptr;
    ASSERT_EQ(bytes[0], 0xCD);
    ASSERT_EQ(bytes[initial_size - 1], 0xCD);

    // 写入新区域
    memset((unsigned char*)new_ptr + initial_size, 0xEF, new_size - initial_size);
    ASSERT_EQ(bytes[initial_size], 0xEF);
    ASSERT_EQ(bytes[new_size - 1], 0xEF);

    woomem_free(new_ptr);
    return true;
}

bool test_basic_free() {
    void* ptr = woomem_alloc_normal(50);
    ASSERT_NOT_NULL(ptr);
    woomem_free(ptr);
    // 正常情况下不应该崩溃
    return true;
}

bool test_zero_allocation() {
    void* ptr = woomem_alloc_normal(0);
    ASSERT_NOT_NULL(ptr);
    woomem_free(ptr);
    return true;
}

// ==================== 边界条件测试 ====================

bool test_large_allocation() {
    size_t large_size = 1024 * 1024; // 1MB
    void* ptr = woomem_alloc_normal(large_size);
    ASSERT_NOT_NULL(ptr);

    // 写入并验证
    memset(ptr, 0x12, large_size);
    unsigned char* bytes = (unsigned char*)ptr;
    ASSERT_EQ(bytes[0], 0x12);
    ASSERT_EQ(bytes[large_size / 2], 0x12);
    ASSERT_EQ(bytes[large_size - 1], 0x12);

    woomem_free(ptr);
    return true;
}

bool test_multiple_realloc() {
    void* ptr = woomem_alloc_normal(100);
    ASSERT_NOT_NULL(ptr);

    // 多次调整大小
    ptr = woomem_realloc(ptr, 200);
    ASSERT_NOT_NULL(ptr);

    ptr = woomem_realloc(ptr, 150);
    ASSERT_NOT_NULL(ptr);

    ptr = woomem_realloc(ptr, 300);
    ASSERT_NOT_NULL(ptr);

    ptr = woomem_realloc(ptr, 50);
    ASSERT_NOT_NULL(ptr);

    woomem_free(ptr);
    return true;
}

bool test_realloc_expand() {
    size_t initial = 50;
    void* ptr = woomem_alloc_normal(initial);
    ASSERT_NOT_NULL(ptr);

    memset(ptr, 0x33, initial);

    // 扩大
    size_t expanded = 100;
    ptr = woomem_realloc(ptr, expanded);
    ASSERT_NOT_NULL(ptr);

    // 验证原数据
    unsigned char* bytes = (unsigned char*)ptr;
    ASSERT_EQ(bytes[0], 0x33);
    ASSERT_EQ(bytes[initial - 1], 0x33);

    woomem_free(ptr);
    return true;
}

bool test_realloc_shrink() {
    size_t initial = 200;
    void* ptr = woomem_alloc_normal(initial);
    ASSERT_NOT_NULL(ptr);

    memset(ptr, 0x44, initial);

    // 缩小
    size_t shrunk = 100;
    ptr = woomem_realloc(ptr, shrunk);
    ASSERT_NOT_NULL(ptr);

    // 验证保留的数据
    unsigned char* bytes = (unsigned char*)ptr;
    ASSERT_EQ(bytes[0], 0x44);
    ASSERT_EQ(bytes[shrunk - 1], 0x44);

    woomem_free(ptr);
    return true;
}

bool test_realloc_to_zero() {
    void* ptr = woomem_alloc_normal(100);
    ASSERT_NOT_NULL(ptr);

    // realloc 到 0 可能返回 NULL 或释放内存
    void* new_ptr = woomem_realloc(ptr, 0);
    if (new_ptr != nullptr) {
        woomem_free(new_ptr);
    }
    return true;
}

bool test_very_small_allocations() {
    for (size_t i = 1; i <= 100; i++) {
        void* ptr = woomem_alloc_normal(i);
        ASSERT_NOT_NULL(ptr);

        // 写入数据
        memset(ptr, 0x55, i);
        unsigned char* bytes = (unsigned char*)ptr;
        ASSERT_EQ(bytes[0], 0x55);
        ASSERT_EQ(bytes[i - 1], 0x55);

        woomem_free(ptr);
    }
    return true;
}

// ==================== 单线程场景测试 ====================

bool test_many_small_allocations() {
    const int count = 1000;
    std::vector<void*> pointers;

    for (int i = 0; i < count; i++) {
        void* ptr = woomem_alloc_normal(64);
        ASSERT_NOT_NULL(ptr);
        pointers.push_back(ptr);
    }

    // 验证所有指针都有效
    for (int i = 0; i < count; i++) {
        memset(pointers[i], 0x66 + (i % 256), 64);
    }

    // 释放所有
    for (int i = 0; i < count; i++) {
        woomem_free(pointers[i]);
    }
    return true;
}

bool test_mixed_sizes() {
    std::vector<void*> pointers;
    std::vector<size_t> sizes = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };

    // 分配不同大小
    for (size_t size : sizes) {
        for (int i = 0; i < 10; i++) {
            void* ptr = woomem_alloc_normal(size);
            ASSERT_NOT_NULL(ptr);
            memset(ptr, 0x77, size);
            pointers.push_back(ptr);
        }
    }

    // 释放所有
    for (void* ptr : pointers) {
        woomem_free(ptr);
    }
    return true;
}

bool test_allocation_cycle() {
    const int iterations = 100;

    for (int i = 0; i < iterations; i++) {
        // 分配
        void* ptr1 = woomem_alloc_normal(100);
        void* ptr2 = woomem_alloc_normal(200);
        void* ptr3 = woomem_alloc_normal(50);

        ASSERT_NOT_NULL(ptr1);
        ASSERT_NOT_NULL(ptr2);
        ASSERT_NOT_NULL(ptr3);

        // 使用
        memset(ptr1, 0x11, 100);
        memset(ptr2, 0x22, 200);
        memset(ptr3, 0x33, 50);

        // 释放
        woomem_free(ptr1);
        woomem_free(ptr2);
        woomem_free(ptr3);
    }
    return true;
}

bool test_data_integrity() {
    size_t size = 1024;
    void* ptr = woomem_alloc_normal(size);
    ASSERT_NOT_NULL(ptr);

    // 写入特定模式
    unsigned char* bytes = (unsigned char*)ptr;
    for (size_t i = 0; i < size; i++) {
        bytes[i] = static_cast<unsigned char>(i % 256);
    }

    // 验证
    for (size_t i = 0; i < size; i++) {
        ASSERT_EQ(bytes[i], static_cast<unsigned char>(i % 256));
    }

    // realloc 后验证
    void* new_ptr = woomem_realloc(ptr, size * 2);
    ASSERT_NOT_NULL(new_ptr);
    bytes = (unsigned char*)new_ptr;

    for (size_t i = 0; i < size; i++) {
        ASSERT_EQ(bytes[i], static_cast<unsigned char>(i % 256));
    }

    woomem_free(new_ptr);
    return true;
}

bool test_random_pattern() {
    std::mt19937 rng(42); // 固定种子
    std::uniform_int_distribution<size_t> size_dist(1, 4096);
    std::uniform_int_distribution<int> action_dist(0, 2);

    std::vector<void*> pointers;

    for (int i = 0; i < 1000; i++) {
        int action = action_dist(rng);

        if (action == 0 && !pointers.empty()) {
            // 释放
            void* ptr = pointers.back();
            pointers.pop_back();
            woomem_free(ptr);
        }
        else if (action == 1 && !pointers.empty()) {
            // realloc
            size_t idx = rng() % pointers.size();
            size_t new_size = size_dist(rng);
            pointers[idx] = woomem_realloc(pointers[idx], new_size);
            ASSERT_NOT_NULL(pointers[idx]);
        }
        else {
            // 分配
            size_t size = size_dist(rng);
            void* ptr = woomem_alloc_normal(size);
            ASSERT_NOT_NULL(ptr);
            pointers.push_back(ptr);
        }
    }

    // 清理剩余
    for (void* ptr : pointers) {
        woomem_free(ptr);
    }
    return true;
}

// ==================== 多线程并发测试 ====================

void concurrent_alloc_worker(int thread_id, int num_allocs) {
    for (int i = 0; i < num_allocs; i++) {
        void* ptr = woomem_alloc_normal(128);
        if (ptr != nullptr) {
            memset(ptr, thread_id & 0xFF, 128);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            woomem_free(ptr);
        }
    }
}

bool test_concurrent_allocations() {
    const int num_threads = 8;
    const int num_allocs = 100;

    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(concurrent_alloc_worker, i, num_allocs);
    }

    for (auto& t : threads) {
        t.join();
    }
    return true;
}

void concurrent_realloc_worker(int thread_id) {
    void* ptr = woomem_alloc_normal(100);
    if (ptr == nullptr) return;

    memset(ptr, thread_id & 0xFF, 100);

    for (int i = 0; i < 50; i++) {
        size_t new_size = 100 + i * 10;
        ptr = woomem_realloc(ptr, new_size);
        if (ptr != nullptr) {
            memset((unsigned char*)ptr + 100, thread_id & 0xFF, new_size - 100);
        }
    }

    if (ptr != nullptr) {
        woomem_free(ptr);
    }
}

bool test_concurrent_realloc() {
    const int num_threads = 8;

    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(concurrent_realloc_worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }
    return true;
}

bool test_mixed_concurrent_operations() {
    const int num_threads = 16;
    const int operations = 100;

    auto worker = [&](int thread_id) {
        std::mt19937 rng(thread_id);
        std::uniform_int_distribution<int> action_dist(0, 2);

        for (int i = 0; i < operations; i++) {
            int action = action_dist(rng);

            if (action == 0) {
                void* ptr = woomem_alloc_normal(64);
                if (ptr) {
                    memset(ptr, thread_id & 0xFF, 64);
                    woomem_free(ptr);
                }
            }
            else if (action == 1) {
                void* ptr = woomem_alloc_normal(100);
                if (ptr) {
                    ptr = woomem_realloc(ptr, 200);
                    if (ptr) {
                        woomem_free(ptr);
                    }
                }
            }
            else {
                void* ptr = woomem_alloc_normal(256);
                if (ptr) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                    woomem_free(ptr);
                }
            }
        }
        };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }
    return true;
}

void stress_test_worker(int thread_id, int iterations) {
    for (int i = 0; i < iterations; i++) {
        void* ptr = woomem_alloc_normal(512);
        if (ptr) {
            memset(ptr, thread_id & 0xFF, 512);

            // 随机决定是否 realloc
            if (i % 3 == 0) {
                ptr = woomem_realloc(ptr, 1024);
                if (ptr) {
                    memset((unsigned char*)ptr + 512, (thread_id + 1) & 0xFF, 512);
                }
            }

            woomem_free(ptr);
        }
    }
}

bool test_high_concurrency() {
    const int num_threads = 32;
    const int iterations = 200;

    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(stress_test_worker, i, iterations);
    }

    for (auto& t : threads) {
        t.join();
    }
    return true;
}

// ==================== 压力测试 ====================

bool test_massive_allocations() {
    const int count = 10000;
    std::vector<void*> pointers;

    for (int i = 0; i < count; i++) {
        void* ptr = woomem_alloc_normal(128);
        if (ptr != nullptr) {
            pointers.push_back(ptr);
        }
    }

    // 验证
    for (size_t i = 0; i < pointers.size(); i++) {
        memset(pointers[i], 0x88, 128);
    }

    // 释放
    for (void* ptr : pointers) {
        woomem_free(ptr);
    }
    return true;
}

bool test_rapid_alloc_free() {
    const int iterations = 50000;

    for (int i = 0; i < iterations; i++) {
        void* ptr = woomem_alloc_normal(64);
        if (ptr) {
            woomem_free(ptr);
        }
    }
    return true;
}

bool test_memory_fragmentation() {
    std::vector<void*> pointers;

    // 分配不同大小的块
    for (int i = 0; i < 1000; i++) {
        size_t size = (i % 10 + 1) * 64;
        void* ptr = woomem_alloc_normal(size);
        if (ptr) {
            pointers.push_back(ptr);
        }
    }

    // 随机释放一半
    for (size_t i = 0; i < pointers.size(); i++) {
        if (i % 2 == 0) {
            woomem_free(pointers[i]);
            pointers[i] = nullptr;
        }
    }

    // 再次分配
    for (int i = 0; i < 500; i++) {
        void* ptr = woomem_alloc_normal(128);
        if (ptr) {
            pointers.push_back(ptr);
        }
    }

    // 释放所有
    for (void* ptr : pointers) {
        if (ptr != nullptr) {
            woomem_free(ptr);
        }
    }
    return true;
}

// ==================== 主函数 ====================

int main(void) {
    std::cout << "========================================" << std::endl;
    std::cout << "WooMem Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    // 初始化内存分配器
    std::cout << "\nInitializing woomem..." << std::endl;
    woomem_init(nullptr, nullptr, nullptr);
    std::cout << "woomem_init completed." << std::endl;

    std::cout << "\n========================================" << std::endl;
    std::cout << "Running Tests..." << std::endl;
    std::cout << "========================================\n" << std::endl;

    // 基础功能测试
    std::cout << "--- Basic Function Tests ---" << std::endl;
    TEST(test_basic_allocation);
    TEST(test_basic_realloc);
    TEST(test_basic_free);
    TEST(test_zero_allocation);

    // 边界条件测试
    std::cout << "\n--- Boundary Condition Tests ---" << std::endl;
    TEST(test_large_allocation);
    TEST(test_multiple_realloc);
    TEST(test_realloc_expand);
    TEST(test_realloc_shrink);
    TEST(test_realloc_to_zero);
    TEST(test_very_small_allocations);

    // 单线程场景测试
    std::cout << "\n--- Single-Thread Scenario Tests ---" << std::endl;
    TEST(test_many_small_allocations);
    TEST(test_mixed_sizes);
    TEST(test_allocation_cycle);
    TEST(test_data_integrity);
    TEST(test_random_pattern);

    // 多线程并发测试
    std::cout << "\n--- Multi-Thread Concurrency Tests ---" << std::endl;
    TEST(test_concurrent_allocations);
    TEST(test_concurrent_realloc);
    TEST(test_mixed_concurrent_operations);
    TEST(test_high_concurrency);

    // 压力测试
    std::cout << "\n--- Stress Tests ---" << std::endl;
    TEST(test_massive_allocations);
    TEST(test_rapid_alloc_free);
    TEST(test_memory_fragmentation);

    // 清理
    std::cout << "\n========================================" << std::endl;
    std::cout << "Cleaning up..." << std::endl;
    woomem_shutdown();
    std::cout << "woomem_shutdown completed." << std::endl;

    // 输出结果
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Total:   " << stats.total << std::endl;
    std::cout << "Passed:  " << stats.passed << std::endl;
    std::cout << "Failed:  " << stats.failed << std::endl;

    if (stats.failed == 0) {
        std::cout << "\nAll tests passed!" << std::endl;
        return 0;
    }
    else {
        std::cout << "\nSome tests failed!" << std::endl;
        return 1;
    }
}
