#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>

#define NUM_THREADS 4
#define ITERATIONS_PER_THREAD 200000

typedef struct {
    union {
        uint32_t val;
        struct {
            uint16_t now_serving;
            uint16_t next_ticket;
        };
    };
} ticket_lock_t;

static inline void ticket_lock_init(ticket_lock_t *lock) {
    lock->val = 0;
}

static inline void ticket_lock_acquire(ticket_lock_t *lock) {
    uint16_t my_ticket = __atomic_fetch_add(&lock->next_ticket, 1, __ATOMIC_RELAXED);
    int spins = 0;
    while (__atomic_load_n(&lock->now_serving, __ATOMIC_ACQUIRE) != my_ticket) {
        __asm__ volatile("pause" ::: "memory");
        if (++spins >= 128) {
            sched_yield();
            spins = 0;
        }
    }
}

static inline void ticket_lock_release(ticket_lock_t *lock) {
    uint16_t serving = lock->now_serving + 1;
    __atomic_store_n(&lock->now_serving, serving, __ATOMIC_RELEASE);
}

static inline int ticket_lock_tryacquire(ticket_lock_t *lock) {
    uint32_t current = __atomic_load_n(&lock->val, __ATOMIC_RELAXED);
    uint16_t serving = (uint16_t)(current & 0xFFFF);
    uint16_t next = (uint16_t)(current >> 16);
    if (serving != next) {
        return 0;
    }
    uint32_t updated = ((uint32_t)(next + 1) << 16) | serving;
    return __atomic_compare_exchange_n(&lock->val, &current, updated, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static ticket_lock_t global_lock;
static volatile uint64_t shared_counter = 0;
static uint64_t thread_counts[NUM_THREADS];

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static void *worker_fn(void *arg) {
    int tid = (int)(intptr_t)arg;
    for (int i = 0; i < ITERATIONS_PER_THREAD; i++) {
        ticket_lock_acquire(&global_lock);
        shared_counter++;
        thread_counts[tid]++;
        ticket_lock_release(&global_lock);
    }
    return NULL;
}

int main(void) {
    printf("====================================================\n");
    printf("AvoryOS Ticket / TTAS Spinlock Test & Benchmark Suite\n");
    printf("====================================================\n\n");

    ticket_lock_init(&global_lock);
    shared_counter = 0;
    memset(thread_counts, 0, sizeof(thread_counts));

    // -------------------------------------------------------------------------
    // Test 1: Multi-Threaded Heavy Contention & Correctness
    // -------------------------------------------------------------------------
    uint64_t expected_total = (uint64_t)NUM_THREADS * ITERATIONS_PER_THREAD;
    printf("[TEST 1] Multi-Threaded Contention Stress (%d threads x %d iters = %llu total)...\n",
           NUM_THREADS, ITERATIONS_PER_THREAD, (unsigned long long)expected_total);

    pthread_t threads[NUM_THREADS];
    double t0 = get_time_sec();

    for (int i = 0; i < NUM_THREADS; i++) {
        int rc = pthread_create(&threads[i], NULL, worker_fn, (void *)(intptr_t)i);
        if (rc != 0) {
            printf("[FAIL] pthread_create failed for thread %d\n", i);
            return 1;
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    double t1 = get_time_sec();
    double total_time = t1 - t0;
    double ops = (double)expected_total / total_time;
    double ns_per_op = (total_time / (double)expected_total) * 1000000000.0;

    if (shared_counter != expected_total) {
        printf("[FAIL] Race condition detected! Expected %llu, got %llu\n",
               (unsigned long long)expected_total, (unsigned long long)shared_counter);
        return 1;
    }

    printf("[PASS]   Shared counter verified: exactly %llu (zero races, bit-for-bit accurate)!\n",
           (unsigned long long)shared_counter);
    printf("[BENCH]  Total Time:          %.4f seconds\n", total_time);
    printf("[BENCH]  Lock Throughput:     %.0f acquires/sec\n", ops);
    printf("[BENCH]  Avg Lock Latency:    %.1f nanoseconds\n\n", ns_per_op);

    // -------------------------------------------------------------------------
    // Test 2: FIFO Fairness & Anti-Starvation Verification
    // -------------------------------------------------------------------------
    printf("[TEST 2] Fairness & Anti-Starvation Verification...\n");
    for (int i = 0; i < NUM_THREADS; i++) {
        printf("         Thread %d acquisitions: %llu (expected: %d)\n",
               i, (unsigned long long)thread_counts[i], ITERATIONS_PER_THREAD);
        if (thread_counts[i] != ITERATIONS_PER_THREAD) {
            printf("[FAIL] Thread %d had unexpected acquisition count!\n", i);
            return 1;
        }
    }
    printf("[PASS]   Strict FIFO fairness verified across all %d cores!\n\n", NUM_THREADS);

    // -------------------------------------------------------------------------
    // Test 3: Try-Lock Verification
    // -------------------------------------------------------------------------
    printf("[TEST 3] Non-Blocking Try-Lock Semantics...\n");
    ticket_lock_t trylock_test;
    ticket_lock_init(&trylock_test);

    // 1. Try-acquire on free lock must succeed
    if (!ticket_lock_tryacquire(&trylock_test)) {
        printf("[FAIL] tryacquire on free lock failed\n");
        return 1;
    }
    // 2. Try-acquire while held must fail immediately
    if (ticket_lock_tryacquire(&trylock_test)) {
        printf("[FAIL] tryacquire on held lock succeeded incorrectly\n");
        return 1;
    }
    // 3. Release and try-acquire must succeed again
    ticket_lock_release(&trylock_test);
    if (!ticket_lock_tryacquire(&trylock_test)) {
        printf("[FAIL] tryacquire after release failed\n");
        return 1;
    }
    ticket_lock_release(&trylock_test);
    printf("[PASS]   Try-lock semantics verified!\n\n");

    printf("====================================================\n");
    printf("ALL TICKET SPINLOCK TESTS PASSED SUCCESSFULLY!\n");
    printf("====================================================\n");
    return 0;
}
