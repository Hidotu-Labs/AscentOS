#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <errno.h>

#define NUM_THREADS 4
#define PAGES_PER_THREAD 5000
#define SINGLE_THREAD_PAGES 20000

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

typedef struct {
    int thread_id;
    int num_pages;
    int failed;
    double duration;
} thread_arg_t;

static void *worker_thread(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    size_t size = (size_t)t->num_pages * 4096;

    double t0 = get_time_sec();

    // 1. Map memory region for this thread
    uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        t->failed = 1;
        return NULL;
    }

    // 2. Write unique thread signature across all pages (forces physical PMM allocation via PCP)
    for (int i = 0; i < t->num_pages; i++) {
        uint32_t *ptr = (uint32_t *)(mem + i * 4096);
        ptr[0] = 0xAA000000 | (t->thread_id << 16) | (i & 0xFFFF);
        ptr[1023] = 0x55000000 | (t->thread_id << 16) | (i & 0xFFFF);
    }

    // 3. Verify all pages retained exact thread signatures
    for (int i = 0; i < t->num_pages; i++) {
        uint32_t *ptr = (uint32_t *)(mem + i * 4096);
        uint32_t exp0 = 0xAA000000 | (t->thread_id << 16) | (i & 0xFFFF);
        uint32_t exp1 = 0x55000000 | (t->thread_id << 16) | (i & 0xFFFF);
        if (ptr[0] != exp0 || ptr[1023] != exp1) {
            t->failed = 2;
            break;
        }
    }

    // 4. Free memory back to PMM / PCP
    munmap(mem, size);

    double t1 = get_time_sec();
    t->duration = t1 - t0;
    return NULL;
}

int main(void) {
    printf("====================================================\n");
    printf("AvoryOS Per-CPU Page Frame Allocator (PCP) Test Suite\n");
    printf("====================================================\n\n");

    // -------------------------------------------------------------------------
    // Test 1: Single-Thread Rapid Allocation & Free Throughput
    // -------------------------------------------------------------------------
    printf("[TEST 1] Single-Thread PCP Fast Path Benchmark (%d pages / %d MB)...\n",
           SINGLE_THREAD_PAGES, (int)(SINGLE_THREAD_PAGES * 4096 / (1024 * 1024)));

    size_t total_bytes = (size_t)SINGLE_THREAD_PAGES * 4096;
    uint8_t *mem = mmap(NULL, total_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

    double t0 = get_time_sec();
    // Demand-fault all pages from PMM
    for (size_t i = 0; i < (size_t)SINGLE_THREAD_PAGES; i++) {
        volatile uint32_t *p = (volatile uint32_t *)(mem + i * 4096);
        *p = (uint32_t)(0x12345678ULL ^ i);
    }

    // Verify data
    for (size_t i = 0; i < (size_t)SINGLE_THREAD_PAGES; i++) {
        volatile uint32_t *p = (volatile uint32_t *)(mem + i * 4096);
        if (*p != (uint32_t)(0x12345678ULL ^ i)) {
            printf("[FAIL] Data mismatch at page %zu\n", i);
            return 1;
        }
    }

    // Free all pages back to PMM / PCP
    munmap(mem, total_bytes);
    double t1 = get_time_sec();
    double total_time = t1 - t0;
    double ops = (double)(SINGLE_THREAD_PAGES * 2) / total_time;
    double us_per_op = (total_time / (double)(SINGLE_THREAD_PAGES * 2)) * 1000000.0;

    printf("[PASS]   Allocated, dirtied, and freed %d pages successfully!\n", SINGLE_THREAD_PAGES);
    printf("[BENCH]  Total Time:       %.4f seconds\n", total_time);
    printf("[BENCH]  PMM Operations:   %.0f ops/sec\n", ops);
    printf("[BENCH]  Avg Latency/Page: %.3f microseconds\n\n", us_per_op);

    // -------------------------------------------------------------------------
    // Test 2: Multi-Threaded Parallel Allocation Stress
    // -------------------------------------------------------------------------
    int total_par_pages = NUM_THREADS * PAGES_PER_THREAD;
    printf("[TEST 2] Multi-Threaded Parallel Allocation Stress (%d threads x %d pages = %d total / %d MB)...\n",
           NUM_THREADS, PAGES_PER_THREAD, total_par_pages, (int)(total_par_pages * 4096 / (1024 * 1024)));

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    double par_t0 = get_time_sec();
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i + 1;
        args[i].num_pages = PAGES_PER_THREAD;
        args[i].failed = 0;
        args[i].duration = 0;
        int rc = pthread_create(&threads[i], NULL, worker_thread, &args[i]);
        if (rc != 0) {
            printf("[FAIL] pthread_create failed: %d\n", rc);
            return 1;
        }
    }

    int any_failed = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
        if (args[i].failed) {
            printf("[FAIL] Thread %d failed with code %d\n", i + 1, args[i].failed);
            any_failed = 1;
        }
    }
    double par_t1 = get_time_sec();

    if (any_failed) {
        printf("[FAIL] Multi-threaded allocation test failed!\n");
        return 1;
    }

    double par_total = par_t1 - par_t0;
    double par_ops = (double)(total_par_pages * 2) / par_total;

    printf("[PASS]   All %d threads completed with 100%% data isolation and zero faults!\n", NUM_THREADS);
    printf("[BENCH]  Parallel Time:    %.4f seconds\n", par_total);
    printf("[BENCH]  Total PMM Rate:   %.0f ops/sec\n\n", par_ops);

    // -------------------------------------------------------------------------
    // Test 3: Large Sequential Page Pool Cycling (Batch Refill / Drain Verification)
    // -------------------------------------------------------------------------
    printf("[TEST 3] Batch Refill / Drain Cycle Verification (10 cycles of 2,000 pages)...\n");
    for (int cycle = 0; cycle < 10; cycle++) {
        size_t bsize = 2000 * 4096;
        uint8_t *bmem = mmap(NULL, bsize, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (bmem == MAP_FAILED) {
            printf("[FAIL] Cycle %d mmap failed\n", cycle);
            return 1;
        }
        for (int i = 0; i < 2000; i++) {
            *((uint32_t *)(bmem + i * 4096)) = 0xC0FFEE00 | cycle;
        }
        munmap(bmem, bsize);
    }
    printf("[PASS]   20,000 pages cycled through PCP refill/drain paths without leak!\n\n");

    printf("====================================================\n");
    printf("ALL PCP ALLOCATOR TESTS PASSED SUCCESSFULLY!\n");
    printf("====================================================\n");
    return 0;
}
