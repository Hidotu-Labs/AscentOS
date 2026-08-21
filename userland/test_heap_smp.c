// AvoryOS Fine-Grained Slab Cache & SMP Heap Allocator Benchmark
// Tests correctness and multi-threaded scaling of kernel memory allocation.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static volatile uint64_t g_tsc_khz = 0;
static void calibrate_tsc(void) {
    volatile uint64_t *vdso_data = (volatile uint64_t *)0xFFFFFFFFFF600E00ULL;
    uint64_t khz = vdso_data[1];
    if (khz > 100000 && khz < 10000000)
        g_tsc_khz = khz;
    else
        g_tsc_khz = 3800000;
}

static double tsc_to_ms(uint64_t cycles) {
    return (double)cycles * 1.0 / (double)(g_tsc_khz * 1000.0);
}

// -------------------------------------------------------------
// 1. Correctness Test: Multi-threaded Alloc/Free & Data Integrity
// -------------------------------------------------------------
#define THREAD_COUNT 4
#define ALLOCS_PER_THREAD 2500

static const size_t test_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
#define NUM_SIZES (sizeof(test_sizes) / sizeof(test_sizes[0]))

struct thread_arg {
    int thread_id;
    int success;
};

static void *worker_correctness(void *arg) {
    struct thread_arg *targ = (struct thread_arg *)arg;
    int tid = targ->thread_id;
    targ->success = 1;

    void *ptrs[ALLOCS_PER_THREAD];
    size_t sizes[ALLOCS_PER_THREAD];

    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        size_t sz = test_sizes[(i + tid) % NUM_SIZES];
        sizes[i] = sz;
        uint8_t *p = (uint8_t *)malloc(sz);
        if (!p) {
            printf("[FAIL] Thread %d malloc failed at iter %d\n", tid, i);
            targ->success = 0;
            return NULL;
        }
        // Fill with deterministic pattern
        uint8_t pattern = (uint8_t)((tid * 37 + i * 13) & 0xFF);
        memset(p, pattern, sz);
        ptrs[i] = p;
    }

    // Verify all patterns
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        uint8_t *p = (uint8_t *)ptrs[i];
        size_t sz = sizes[i];
        uint8_t pattern = (uint8_t)((tid * 37 + i * 13) & 0xFF);
        for (size_t b = 0; b < sz; b++) {
            if (p[b] != pattern) {
                printf("[FAIL] Thread %d data corruption at iter %d byte %zu\n", tid, i, b);
                targ->success = 0;
                break;
            }
        }
        free(p);
    }

    return NULL;
}

static int test_correctness(void) {
    printf("\n[1] Multi-Threaded Correctness & Integrity Test (%d threads x %d allocs):\n",
           THREAD_COUNT, ALLOCS_PER_THREAD);

    pthread_t threads[THREAD_COUNT];
    struct thread_arg args[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; i++) {
        args[i].thread_id = i;
        args[i].success = 0;
        pthread_create(&threads[i], NULL, worker_correctness, &args[i]);
    }

    int all_ok = 1;
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
        if (!args[i].success) all_ok = 0;
    }

    if (all_ok) {
        printf("    [PASS] %d concurrent allocations across all slab size classes verified 100%% intact!\n",
               THREAD_COUNT * ALLOCS_PER_THREAD);
    } else {
        printf("    [FAIL] Correctness test failed!\n");
    }
    return all_ok;
}

// -------------------------------------------------------------
// 2. SMP Throughput & Scaling Benchmark
// -------------------------------------------------------------
#define BENCH_ITERS 50000

struct bench_arg {
    int iters;
};

static void *worker_bench(void *arg) {
    struct bench_arg *barg = (struct bench_arg *)arg;
    int iters = barg->iters;

    for (int i = 0; i < iters; i++) {
        // Interleave different slab classes (32, 64, 128, 256)
        void *p1 = malloc(32);
        void *p2 = malloc(64);
        void *p3 = malloc(128);
        void *p4 = malloc(256);

        free(p1);
        free(p2);
        free(p3);
        free(p4);
    }
    return NULL;
}

static void run_bench(int num_threads) {
    pthread_t threads[num_threads];
    struct bench_arg args[num_threads];
    int iters_per_thread = BENCH_ITERS / num_threads;

    uint64_t t0 = rdtsc();
    for (int i = 0; i < num_threads; i++) {
        args[i].iters = iters_per_thread;
        pthread_create(&threads[i], NULL, worker_bench, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    uint64_t t1 = rdtsc();

    uint64_t total_ops = (uint64_t)BENCH_ITERS * 4 * 2; // 4 allocs + 4 frees per iter
    double elapsed_ms = tsc_to_ms(t1 - t0);
    double ops_per_sec = ((double)total_ops / (elapsed_ms / 1000.0));

    printf("    %d Thread(s): %llu ops in %.2f ms -> %.2f million ops/sec\n",
           num_threads,
           (unsigned long long)total_ops,
           elapsed_ms,
           ops_per_sec / 1000000.0);
}

int main(void) {
    printf("====================================================\n");
    printf("AvoryOS Fine-Grained Slab & SMP Heap Benchmark\n");
    printf("====================================================\n");

    calibrate_tsc();
    printf("\n[INFO] Calibrated TSC: %llu MHz\n",
           (unsigned long long)(g_tsc_khz / 1000));

    int ok = test_correctness();
    if (!ok) {
        printf("\n[ABORT] Correctness test failed!\n");
        return 1;
    }

    printf("\n[2] Benchmarking SMP Concurrent Alloc/Free Throughput (%d total ops):\n",
           BENCH_ITERS * 8);

    run_bench(1);
    run_bench(2);
    run_bench(4);

    printf("\n====================================================\n");
    printf("FINE-GRAINED SLAB CACHE BENCHMARK COMPLETED!\n");
    printf("====================================================\n");
    return 0;
}
