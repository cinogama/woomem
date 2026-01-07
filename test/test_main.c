#include "woomem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#define THREAD_FUNC DWORD WINAPI
#define THREAD_RETURN return 0
typedef HANDLE thread_t;
typedef CRITICAL_SECTION mutex_t;
#define mutex_init(m) InitializeCriticalSection(m)
#define mutex_destroy(m) DeleteCriticalSection(m)
#define mutex_lock(m) EnterCriticalSection(m)
#define mutex_unlock(m) LeaveCriticalSection(m)
#define thread_create(t, func, arg) (*(t) = CreateThread(NULL, 0, func, arg, 0, NULL), *(t) != NULL ? 0 : -1)
#define thread_join(t) WaitForSingleObject(t, INFINITE)
#define sleep_ms(ms) Sleep(ms)
#define atomic_inc(p) InterlockedIncrement((volatile LONG*)(p))
#define atomic_load(p) (*(volatile LONG*)(p))
#else
#include <pthread.h>
#include <unistd.h>
#define THREAD_FUNC void*
#define THREAD_RETURN return NULL
typedef pthread_t thread_t;
typedef pthread_mutex_t mutex_t;
#define mutex_init(m) pthread_mutex_init(m, NULL)
#define mutex_destroy(m) pthread_mutex_destroy(m)
#define mutex_lock(m) pthread_mutex_lock(m)
#define mutex_unlock(m) pthread_mutex_unlock(m)
#define thread_create(t, func, arg) pthread_create(t, NULL, func, arg)
#define thread_join(t) pthread_join(t, NULL)
#define sleep_ms(ms) usleep((ms) * 1000)
#define atomic_inc(p) __sync_fetch_and_add(p, 1)
#define atomic_load(p) __sync_fetch_and_add(p, 0)
#endif

static volatile long g_destroy_count = 0;
static volatile long g_test_failures = 0;
static mutex_t g_print_mutex;

#define TEST_ASSERT(cond, msg) do { if (!(cond)) { mutex_lock(&g_print_mutex); printf("  FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__); mutex_unlock(&g_print_mutex); atomic_inc(&g_test_failures); } } while(0)
#define TEST_ASSERT_EQ(a, b, msg) do { if ((a) != (b)) { mutex_lock(&g_print_mutex); printf("  FAIL: %s (got %ld, want %ld)\n", msg, (long)(a), (long)(b)); mutex_unlock(&g_print_mutex); atomic_inc(&g_test_failures); } } while(0)

void test_destroy_func(void* ptr, void* userdata) { (void)ptr; (void)userdata; atomic_inc(&g_destroy_count); }

void test_basic_alloc(void) {
    printf("Testing basic allocation...\n");
    void* ptr = woomem_alloc(100);
    TEST_ASSERT(ptr != NULL, "alloc");
    memset(ptr, 0xAB, 100);
    woomem_free(ptr);
    printf("  Basic allocation: PASSED\n");
}

void test_various_sizes(void) {
    printf("Testing various sizes...\n");
    size_t sizes[] = {1, 8, 16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536};
    int n = sizeof(sizes)/sizeof(sizes[0]);
    void** ptrs = (void**)malloc(n * sizeof(void*));
    for (int i = 0; i < n; i++) { ptrs[i] = woomem_alloc(sizes[i]); TEST_ASSERT(ptrs[i] != NULL, "alloc"); }
    for (int i = n-1; i >= 0; i--) woomem_free(ptrs[i]);
    free(ptrs);
    printf("  Various sizes: PASSED\n");
}

void test_gc_mark_and_free(void) {
    printf("Testing GC mark and free...\n");
    g_destroy_count = 0;
    void* p1 = woomem_alloc(64); void* p2 = woomem_alloc(128); void* p3 = woomem_alloc(256);
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    void* m1 = woomem_try_mark_self((intptr_t)p1); void* m3 = woomem_try_mark_self((intptr_t)p3);
    TEST_ASSERT(m1 != NULL, "m1"); TEST_ASSERT(m3 != NULL, "m3");
    woomem_full_mark(p1); woomem_full_mark(p3);
    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
    TEST_ASSERT_EQ(atomic_load(&g_destroy_count), 1, "one destroyed");
    g_destroy_count = 0;
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    m1 = woomem_try_mark_self((intptr_t)p1); woomem_full_mark(p1);
    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
    TEST_ASSERT_EQ(atomic_load(&g_destroy_count), 1, "p3 destroyed");
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
    printf("  GC mark and free: PASSED\n");
}

void test_alloc_during_gc(void) {
    printf("Testing alloc during GC...\n");
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE); woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
    g_destroy_count = 0;
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    void* ptr = woomem_alloc(64); TEST_ASSERT(ptr != NULL, "alloc during gc");
    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
    TEST_ASSERT_EQ(atomic_load(&g_destroy_count), 0, "protected");
    g_destroy_count = 0;
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    woomem_end_gc_mark_and_free_all_unmarked(test_destroy_func, NULL);
    TEST_ASSERT_EQ(atomic_load(&g_destroy_count), 1, "collected");
    printf("  Alloc during GC: PASSED\n");
}

void test_double_mark(void) {
    printf("Testing double mark...\n");
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE); woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
    void* ptr = woomem_alloc(64);
    woomem_begin_gc_mark(WOOMEM_BOOL_TRUE);
    void* f = woomem_try_mark_self((intptr_t)ptr);
    void* s = woomem_try_mark_self((intptr_t)ptr);
    TEST_ASSERT(f != NULL, "first"); TEST_ASSERT(s == NULL, "second");
    woomem_full_mark(ptr);
    woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
    printf("  Double mark: PASSED\n");
}

typedef struct { int id; int iters; int size; } ThreadData;

THREAD_FUNC concurrent_thread(void* arg) {
    ThreadData* d = (ThreadData*)arg;
    for (int i = 0; i < d->iters; i++) {
        void* p = woomem_alloc(d->size + (i % 100));
        if (!p) { atomic_inc(&g_test_failures); THREAD_RETURN; }
        memset(p, d->id, d->size);
        woomem_free(p);
    }
    THREAD_RETURN;
}

void test_concurrent(void) {
    printf("Testing concurrent alloc/free...\n");
    thread_t threads[8]; ThreadData data[8];
    for (int i = 0; i < 8; i++) { data[i].id = i; data[i].iters = 10000; data[i].size = 64; thread_create(&threads[i], concurrent_thread, &data[i]); }
    for (int i = 0; i < 8; i++) thread_join(threads[i]);
    printf("  Concurrent: PASSED\n");
}

static volatile int g_gc_running = 0;

THREAD_FUNC gc_thread(void* arg) {
    ThreadData* d = (ThreadData*)arg;
    while (atomic_load((volatile long*)&g_gc_running)) {
        void* p = woomem_alloc(d->size);
        if (p) { *(int*)p = d->id; woomem_free(p); }
    }
    THREAD_RETURN;
}

void test_concurrent_gc(void) {
    printf("Testing concurrent GC...\n");
    thread_t threads[4]; ThreadData data[4]; g_gc_running = 1;
    for (int i = 0; i < 4; i++) { data[i].id = i; data[i].size = 64; thread_create(&threads[i], gc_thread, &data[i]); }
    for (int r = 0; r < 20; r++) {
        woomem_begin_gc_mark(r % 3 == 0 ? WOOMEM_BOOL_TRUE : WOOMEM_BOOL_FALSE);
        sleep_ms(5);
        woomem_end_gc_mark_and_free_all_unmarked(NULL, NULL);
        sleep_ms(5);
    }
    g_gc_running = 0;
    for (int i = 0; i < 4; i++) thread_join(threads[i]);
    printf("  Concurrent GC: PASSED\n");
}

THREAD_FUNC cleanup_thread(void* arg) {
    (void)arg;
    void* ptrs[100];
    for (int i = 0; i < 100; i++) ptrs[i] = woomem_alloc(64);
    for (int i = 0; i < 100; i++) woomem_free(ptrs[i]);
    THREAD_RETURN;
}

void test_cleanup(void) {
    printf("Testing thread cleanup...\n");
    thread_t threads[8];
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 8; i++) thread_create(&threads[i], cleanup_thread, NULL);
        for (int i = 0; i < 8; i++) thread_join(threads[i]);
    }
    printf("  Thread cleanup: PASSED\n");
}

void test_stress(void) {
    printf("Testing stress...\n");
    void** pool = (void**)calloc(1000, sizeof(void*)); int cnt = 0; unsigned seed = 12345;
    for (int i = 0; i < 50000; i++) {
        seed = seed * 1103515245 + 12345;
        if ((seed >> 16) % 2 == 0 || cnt == 0) { if (cnt < 1000) { void* p = woomem_alloc(8 + (seed % 1024)); if (p) pool[cnt++] = p; } }
        else { int idx = (seed >> 8) % cnt; woomem_free(pool[idx]); pool[idx] = pool[--cnt]; }
    }
    for (int i = 0; i < cnt; i++) woomem_free(pool[i]);
    free(pool);
    printf("  Stress: PASSED\n");
}

int main(void) {
    mutex_init(&g_print_mutex);
    printf("=== woomem Test Suite ===\n\n");
    woomem_init();
    
    printf("--- Basic Tests ---\n");
    test_basic_alloc();
    test_various_sizes();
    
    printf("\n--- GC Tests ---\n");
    test_gc_mark_and_free();
    test_alloc_during_gc();
    test_double_mark();
    
    printf("\n--- Concurrency Tests ---\n");
    test_concurrent();
    test_concurrent_gc();
    test_cleanup();
    
    printf("\n--- Stress Tests ---\n");
    test_stress();
    
    woomem_shutdown();
    mutex_destroy(&g_print_mutex);
    
    printf("\n========================================\n");
    if (atomic_load(&g_test_failures) == 0) { printf("=== All tests passed! ===\n"); return 0; }
    else { printf("=== %ld FAILED ===\n", (long)atomic_load(&g_test_failures)); return 1; }
}
