#include "woomem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <pthread.h>
#endif

// ============================================================================
// 防止编译器优化的辅助函数
// ============================================================================

// 使用 volatile 和 noinline 防止编译器优化掉 malloc/free
#ifdef _MSC_VER
__declspec(noinline) void use_pointer(void* p) {
    volatile char* vp = (volatile char*)p;
    (void)*vp;  // 强制读取一个字节
}
#else
__attribute__((noinline)) void use_pointer(void* p) {
    volatile char* vp = (volatile char*)p;
    (void)*vp;  // 强制读取一个字节
}
#endif

// ============================================================================
// 高精度计时器
// ============================================================================

typedef struct {
#ifdef _WIN32
    LARGE_INTEGER start;
    LARGE_INTEGER freq;
#else
    struct timespec start;
#endif
} Timer;

void timer_start(Timer* t) {
#ifdef _WIN32
    QueryPerformanceFrequency(&t->freq);
    QueryPerformanceCounter(&t->start);
#else
    clock_gettime(CLOCK_MONOTONIC, &t->start);
#endif
}

double timer_elapsed_ms(Timer* t) {
#ifdef _WIN32
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    return (double)(end.QuadPart - t->start.QuadPart) * 1000.0 / (double)t->freq.QuadPart;
#else
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (end.tv_sec - t->start.tv_sec) * 1000.0 +
        (end.tv_nsec - t->start.tv_nsec) / 1000000.0;
#endif
}

// ============================================================================
// 测试配置
// ============================================================================

#define SMALL_SIZE      64
#define MEDIUM_SIZE     1024
#define LARGE_SIZE      16384

#define ITERATIONS      1000000
#define BATCH_SIZE      1000

// ============================================================================
// 单线程顺序分配/释放测试
// ============================================================================

void bench_sequential_alloc_free(void)
{
    printf("\n=== Sequential Alloc/Free Benchmark ===\n");
    printf("Iterations: %d\n\n", ITERATIONS);

    Timer t;
    double woomem_time, malloc_time;

    // 测试小对象 (64 bytes)
    {
        printf("Small objects (%d bytes):\n", SMALL_SIZE);

        // woomem
        timer_start(&t);
        for (int i = 0; i < ITERATIONS; ++i) {
            void* p = woomem_alloc_normal(SMALL_SIZE);
            use_pointer(p);
            woomem_free(p);
        }
        woomem_time = timer_elapsed_ms(&t);

        // malloc
        timer_start(&t);
        for (int i = 0; i < ITERATIONS; ++i) {
            void* p = malloc(SMALL_SIZE);
            use_pointer(p);
            free(p);
        }
        malloc_time = timer_elapsed_ms(&t);

        printf("  woomem: %.2f ms (%.2f M ops/sec)\n", woomem_time, ITERATIONS / woomem_time / 1000.0);
        printf("  malloc:  %.2f ms (%.2f M ops/sec)\n", malloc_time, ITERATIONS / malloc_time / 1000.0);
        printf("  Speedup: %.2fx\n\n", malloc_time / woomem_time);
    }

    // 测试中等对象 (1024 bytes)
    {
        printf("Medium objects (%d bytes):\n", MEDIUM_SIZE);

        // woomem
        timer_start(&t);
        for (int i = 0; i < ITERATIONS; ++i) {
            void* p = woomem_alloc_normal(MEDIUM_SIZE);
            use_pointer(p);
            woomem_free(p);
        }
        woomem_time = timer_elapsed_ms(&t);

        // malloc
        timer_start(&t);
        for (int i = 0; i < ITERATIONS; ++i) {
            void* p = malloc(MEDIUM_SIZE);
            use_pointer(p);
            free(p);
        }
        malloc_time = timer_elapsed_ms(&t);

        printf("  woomem: %.2f ms (%.2f M ops/sec)\n", woomem_time, ITERATIONS / woomem_time / 1000.0);
        printf("  malloc:  %.2f ms (%.2f M ops/sec)\n", malloc_time, ITERATIONS / malloc_time / 1000.0);
        printf("  Speedup: %.2fx\n\n", malloc_time / woomem_time);
    }

    // 测试大对象 (16384 bytes)
    {
        int large_iterations = ITERATIONS / 10;  // 大对象测试减少迭代次数
        printf("Large objects (%d bytes, %d iterations):\n", LARGE_SIZE, large_iterations);

        // woomem
        timer_start(&t);
        for (int i = 0; i < large_iterations; ++i) {
            void* p = woomem_alloc_normal(LARGE_SIZE);
            use_pointer(p);
            woomem_free(p);
        }
        woomem_time = timer_elapsed_ms(&t);

        // malloc
        timer_start(&t);
        for (int i = 0; i < large_iterations; ++i) {
            void* p = malloc(LARGE_SIZE);
            use_pointer(p);
            free(p);
        }
        malloc_time = timer_elapsed_ms(&t);

        printf("  woomem: %.2f ms (%.2f M ops/sec)\n", woomem_time, large_iterations / woomem_time / 1000.0);
        printf("  malloc:  %.2f ms (%.2f M ops/sec)\n", malloc_time, large_iterations / malloc_time / 1000.0);
        printf("  Speedup: %.2fx\n\n", malloc_time / woomem_time);
    }
}

// ============================================================================
// 批量分配后批量释放测试
// ============================================================================

void bench_batch_alloc_free(void)
{
    printf("\n=== Batch Alloc then Free Benchmark ===\n");
    printf("Batch size: %d, Iterations: %d\n\n", BATCH_SIZE, ITERATIONS / BATCH_SIZE);

    Timer t;
    double woomem_time, malloc_time;
    void** ptrs = (void**)malloc(BATCH_SIZE * sizeof(void*));

    int batches = ITERATIONS / BATCH_SIZE;

    // 小对象
    {
        printf("Small objects (%d bytes):\n", SMALL_SIZE);

        // woomem
        timer_start(&t);
        for (int b = 0; b < batches; ++b) {
            for (int i = 0; i < BATCH_SIZE; ++i) {
                ptrs[i] = woomem_alloc_normal(SMALL_SIZE);
            }
            for (int i = 0; i < BATCH_SIZE; ++i) {
                woomem_free(ptrs[i]);
            }
        }
        woomem_time = timer_elapsed_ms(&t);

        // malloc
        timer_start(&t);
        for (int b = 0; b < batches; ++b) {
            for (int i = 0; i < BATCH_SIZE; ++i) {
                ptrs[i] = malloc(SMALL_SIZE);
            }
            for (int i = 0; i < BATCH_SIZE; ++i) {
                free(ptrs[i]);
            }
        }
        malloc_time = timer_elapsed_ms(&t);

        printf("  woomem: %.2f ms (%.2f M ops/sec)\n", woomem_time, ITERATIONS / woomem_time / 1000.0);
        printf("  malloc:  %.2f ms (%.2f M ops/sec)\n", malloc_time, ITERATIONS / malloc_time / 1000.0);
        printf("  Speedup: %.2fx\n\n", malloc_time / woomem_time);
    }

    // 中等对象
    {
        printf("Medium objects (%d bytes):\n", MEDIUM_SIZE);

        // woomem
        timer_start(&t);
        for (int b = 0; b < batches; ++b) {
            for (int i = 0; i < BATCH_SIZE; ++i) {
                ptrs[i] = woomem_alloc_normal(MEDIUM_SIZE);
            }
            for (int i = 0; i < BATCH_SIZE; ++i) {
                woomem_free(ptrs[i]);
            }
        }
        woomem_time = timer_elapsed_ms(&t);

        // malloc
        timer_start(&t);
        for (int b = 0; b < batches; ++b) {
            for (int i = 0; i < BATCH_SIZE; ++i) {
                ptrs[i] = malloc(MEDIUM_SIZE);
            }
            for (int i = 0; i < BATCH_SIZE; ++i) {
                free(ptrs[i]);
            }
        }
        malloc_time = timer_elapsed_ms(&t);

        printf("  woomem: %.2f ms (%.2f M ops/sec)\n", woomem_time, ITERATIONS / woomem_time / 1000.0);
        printf("  malloc:  %.2f ms (%.2f M ops/sec)\n", malloc_time, ITERATIONS / malloc_time / 1000.0);
        printf("  Speedup: %.2fx\n\n", malloc_time / woomem_time);
    }

    free(ptrs);
}

// ============================================================================
// 随机大小分配测试
// ============================================================================

void bench_random_sizes(void)
{
    printf("\n=== Random Size Allocation Benchmark ===\n");
    printf("Iterations: %d\n\n", ITERATIONS);

    Timer t;
    double woomem_time, malloc_time;

    // 预生成随机大小
    size_t* sizes = (size_t*)malloc(ITERATIONS * sizeof(size_t));
    srand(12345);  // 固定种子确保可重复
    for (int i = 0; i < ITERATIONS; ++i) {
        // 随机大小：8 - 4096 字节
        sizes[i] = 8 + (rand() % 4089);
    }

    // woomem
    timer_start(&t);
    for (int i = 0; i < ITERATIONS; ++i) {
        void* p = woomem_alloc_normal(sizes[i]);
        use_pointer(p);
        woomem_free(p);
    }
    woomem_time = timer_elapsed_ms(&t);

    // malloc
    timer_start(&t);
    for (int i = 0; i < ITERATIONS; ++i) {
        void* p = malloc(sizes[i]);
        use_pointer(p);
        free(p);
    }
    malloc_time = timer_elapsed_ms(&t);

    printf("Random sizes (8-4096 bytes):\n");
    printf("  woomem: %.2f ms (%.2f M ops/sec)\n", woomem_time, ITERATIONS / woomem_time / 1000.0);
    printf("  malloc:  %.2f ms (%.2f M ops/sec)\n", malloc_time, ITERATIONS / malloc_time / 1000.0);
    printf("  Speedup: %.2fx\n\n", malloc_time / woomem_time);

    free(sizes);
}

// ============================================================================
// 混合分配/释放模式测试（模拟真实工作负载）
// ============================================================================

void bench_mixed_pattern(void)
{
    printf("\n=== Mixed Pattern Benchmark (Simulating Real Workload) ===\n");
    printf("Pool size: %d, Operations: %d\n\n", BATCH_SIZE, ITERATIONS);

    Timer t;
    double woomem_time, malloc_time;

    void** pool = (void**)malloc(BATCH_SIZE * sizeof(void*));
    int pool_count = 0;

    // 预生成随机操作序列
    int* ops = (int*)malloc(ITERATIONS * sizeof(int));  // 0=alloc, 1=free
    size_t* sizes = (size_t*)malloc(ITERATIONS * sizeof(size_t));
    srand(54321);
    for (int i = 0; i < ITERATIONS; ++i) {
        ops[i] = rand() % 2;
        sizes[i] = 16 + (rand() % 1009);  // 16-1024 bytes
    }

    // woomem
    pool_count = 0;
    timer_start(&t);
    for (int i = 0; i < ITERATIONS; ++i) {
        if (ops[i] == 0 || pool_count == 0) {
            // Alloc
            if (pool_count < BATCH_SIZE) {
                pool[pool_count++] = woomem_alloc_normal(sizes[i]);
            }
        }
        else {
            // Free random item
            int idx = rand() % pool_count;
            woomem_free(pool[idx]);
            pool[idx] = pool[--pool_count];
        }
    }
    // 清理剩余
    for (int i = 0; i < pool_count; ++i) {
        woomem_free(pool[i]);
    }
    woomem_time = timer_elapsed_ms(&t);

    // malloc
    pool_count = 0;
    srand(54321);  // 重置随机数以获得相同序列
    timer_start(&t);
    for (int i = 0; i < ITERATIONS; ++i) {
        if (ops[i] == 0 || pool_count == 0) {
            // Alloc
            if (pool_count < BATCH_SIZE) {
                pool[pool_count++] = malloc(sizes[i]);
            }
        }
        else {
            // Free random item
            int idx = rand() % pool_count;
            free(pool[idx]);
            pool[idx] = pool[--pool_count];
        }
    }
    // 清理剩余
    for (int i = 0; i < pool_count; ++i) {
        free(pool[i]);
    }
    malloc_time = timer_elapsed_ms(&t);

    printf("Mixed alloc/free pattern:\n");
    printf("  woomem: %.2f ms (%.2f M ops/sec)\n", woomem_time, ITERATIONS / woomem_time / 1000.0);
    printf("  malloc:  %.2f ms (%.2f M ops/sec)\n", malloc_time, ITERATIONS / malloc_time / 1000.0);
    printf("  Speedup: %.2fx\n\n", malloc_time / woomem_time);

    free(pool);
    free(ops);
    free(sizes);
}

// ============================================================================
// 多线程测试
// ============================================================================

#ifdef _WIN32

typedef struct {
    int thread_id;
    int iterations;
    double woomem_time;
    double malloc_time;
} ThreadData;

DWORD WINAPI thread_bench_woomem(LPVOID arg) {
    ThreadData* data = (ThreadData*)arg;
    Timer t;

    timer_start(&t);
    for (int i = 0; i < data->iterations; ++i) {
        void* p = woomem_alloc_normal(SMALL_SIZE);
        use_pointer(p);
        woomem_free(p);
    }
    data->woomem_time = timer_elapsed_ms(&t);

    return 0;
}

DWORD WINAPI thread_bench_malloc(LPVOID arg) {
    ThreadData* data = (ThreadData*)arg;
    Timer t;

    timer_start(&t);
    for (int i = 0; i < data->iterations; ++i) {
        void* p = malloc(SMALL_SIZE);
        use_pointer(p);
        free(p);
    }
    data->malloc_time = timer_elapsed_ms(&t);

    return 0;
}

void bench_multithread(int num_threads)
{
    printf("\n=== Multi-thread Benchmark (%d threads) ===\n", num_threads);
    printf("Iterations per thread: %d\n\n", ITERATIONS / num_threads);

    HANDLE* threads = (HANDLE*)malloc(num_threads * sizeof(HANDLE));
    ThreadData* data = (ThreadData*)malloc(num_threads * sizeof(ThreadData));

    int iter_per_thread = ITERATIONS / num_threads;
    Timer t;
    double total_woomem, total_malloc;

    // woomem test
    timer_start(&t);
    for (int i = 0; i < num_threads; ++i) {
        data[i].thread_id = i;
        data[i].iterations = iter_per_thread;
        threads[i] = CreateThread(NULL, 0, thread_bench_woomem, &data[i], 0, NULL);
    }
    WaitForMultipleObjects(num_threads, threads, TRUE, INFINITE);
    total_woomem = timer_elapsed_ms(&t);

    for (int i = 0; i < num_threads; ++i) {
        CloseHandle(threads[i]);
    }

    // malloc test
    timer_start(&t);
    for (int i = 0; i < num_threads; ++i) {
        data[i].thread_id = i;
        data[i].iterations = iter_per_thread;
        threads[i] = CreateThread(NULL, 0, thread_bench_malloc, &data[i], 0, NULL);
    }
    WaitForMultipleObjects(num_threads, threads, TRUE, INFINITE);
    total_malloc = timer_elapsed_ms(&t);

    for (int i = 0; i < num_threads; ++i) {
        CloseHandle(threads[i]);
    }

    printf("Small objects (%d bytes):\n", SMALL_SIZE);
    printf("  woomem: %.2f ms (%.2f M ops/sec total)\n", total_woomem, ITERATIONS / total_woomem / 1000.0);
    printf("  malloc:  %.2f ms (%.2f M ops/sec total)\n", total_malloc, ITERATIONS / total_malloc / 1000.0);
    printf("  Speedup: %.2fx\n\n", total_malloc / total_woomem);

    free(threads);
    free(data);
}

#else
// POSIX threads implementation for Unix-like systems
#include <pthread.h>

typedef struct {
    int thread_id;
    int iterations;
    double woomem_time;
    double malloc_time;
} ThreadData;

void* thread_bench_woomem_posix(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    Timer t;

    timer_start(&t);
    for (int i = 0; i < data->iterations; ++i) {
        void* p = woomem_alloc_normal(SMALL_SIZE);
        use_pointer(p);
        woomem_free(p);
    }
    data->woomem_time = timer_elapsed_ms(&t);

    return NULL;
}

void* thread_bench_malloc_posix(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    Timer t;

    timer_start(&t);
    for (int i = 0; i < data->iterations; ++i) {
        void* p = malloc(SMALL_SIZE);
        use_pointer(p);
        free(p);
    }
    data->malloc_time = timer_elapsed_ms(&t);

    return NULL;
}

void bench_multithread(int num_threads)
{
    printf("\n=== Multi-thread Benchmark (%d threads) ===\n", num_threads);
    printf("Iterations per thread: %d\n\n", ITERATIONS / num_threads);

    pthread_t* threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
    ThreadData* data = (ThreadData*)malloc(num_threads * sizeof(ThreadData));

    int iter_per_thread = ITERATIONS / num_threads;
    Timer t;
    double total_woomem, total_malloc;

    // woomem test
    timer_start(&t);
    for (int i = 0; i < num_threads; ++i) {
        data[i].thread_id = i;
        data[i].iterations = iter_per_thread;
        pthread_create(&threads[i], NULL, thread_bench_woomem_posix, &data[i]);
    }
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
    }
    total_woomem = timer_elapsed_ms(&t);

    // malloc test
    timer_start(&t);
    for (int i = 0; i < num_threads; ++i) {
        data[i].thread_id = i;
        data[i].iterations = iter_per_thread;
        pthread_create(&threads[i], NULL, thread_bench_malloc_posix, &data[i]);
    }
    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
    }
    total_malloc = timer_elapsed_ms(&t);

    printf("Small objects (%d bytes):\n", SMALL_SIZE);
    printf("  woomem: %.2f ms (%.2f M ops/sec total)\n", total_woomem, ITERATIONS / total_woomem / 1000.0);
    printf("  malloc:  %.2f ms (%.2f M ops/sec total)\n", total_malloc, ITERATIONS / total_malloc / 1000.0);
    printf("  Speedup: %.2fx\n\n", total_malloc / total_woomem);

    free(threads);
    free(data);
}
#endif

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("  woomem vs malloc Performance Benchmark\n");
    printf("========================================\n");

    woomem_init();

    // 预热
    printf("\nWarming up...\n");
    for (int i = 0; i < 10000; ++i) {
        void* p = woomem_alloc_normal(64);
        woomem_free(p);
        p = malloc(64);
        free(p);
    }

    // 运行各项测试
    bench_sequential_alloc_free();
    bench_batch_alloc_free();
    bench_random_sizes();
    bench_mixed_pattern();

    // 多线程测试
    bench_multithread(2);
    bench_multithread(4);
    bench_multithread(8);

    woomem_shutdown();

    printf("========================================\n");
    printf("  Benchmark Complete!\n");
    printf("========================================\n");

    return 0;
}
