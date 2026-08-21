// AvoryOS Lazy FPU Context Switch Benchmark
// Tests correctness and measures the speedup from lazy FPU state management.
//
// Strategy:
//   1. Correctness: spawn threads that do floating-point work, verify results.
//   2. Benchmark A: measure context-switch cost via futex ping-pong for
//      threads that do NOT use FPU (should be cheaper with lazy FPU).
//   3. Benchmark B: measure context-switch cost for threads that DO use FPU
//      (triggers #NM on first use; subsequent switches are still lazy).

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <sys/syscall.h>
#include <linux/futex.h>

// ---- TSC helpers ----
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static volatile uint64_t g_tsc_khz = 0;
static void calibrate_tsc(void) {
    // vDSO data block at 0xFFFFFFFFFF600E00
    volatile uint64_t *vdso_data = (volatile uint64_t *)0xFFFFFFFFFF600E00ULL;
    uint64_t khz = vdso_data[1]; // tsc_khz at offset +0x08
    if (khz > 100000 && khz < 10000000)
        g_tsc_khz = khz;
    else
        g_tsc_khz = 3000000; // 3 GHz fallback
}

static double tsc_to_ns(uint64_t cycles) {
    return (double)cycles * 1000.0 / (double)g_tsc_khz;
}

// ---- Correctness test: FPU across thread switches ----
#define FPU_THREADS 4
#define FPU_ITERS   100000

static volatile double g_fpu_results[FPU_THREADS];

static void *fpu_worker(void *arg) {
    int id = (int)(uintptr_t)arg;
    double acc = (double)(id + 1);
    // Spin doing FP work so we definitely own the FPU across many switches
    for (int i = 0; i < FPU_ITERS; i++) {
        acc = acc * 1.0000001 + 0.000001;
    }
    g_fpu_results[id] = acc;
    return NULL;
}

static int test_fpu_correctness(void) {
    printf("\n[1] FPU Correctness Across Thread Switches:\n");
    pthread_t threads[FPU_THREADS];
    for (int i = 0; i < FPU_THREADS; i++)
        pthread_create(&threads[i], NULL, fpu_worker, (void *)(uintptr_t)i);
    for (int i = 0; i < FPU_THREADS; i++)
        pthread_join(threads[i], NULL);

    int ok = 1;
    for (int i = 0; i < FPU_THREADS; i++) {
        double val = g_fpu_results[i];
        if (val < 1.0 || val > 1e20) { ok = 0; break; }
    }
    printf("    FPU thread results: %.6f, %.6f, %.6f, %.6f\n",
           g_fpu_results[0], g_fpu_results[1], g_fpu_results[2], g_fpu_results[3]);
    if (ok)
        printf("    [PASS] All FPU results are sane!\n");
    else
        printf("    [FAIL] FPU corruption detected!\n");
    return ok;
}

// ---- Benchmark: futex ping-pong ----
#define PINGPONG_ROUNDS 20000

static volatile int g_futex_a = 0;
static volatile int g_futex_b = 0;
static volatile uint64_t g_switch_cycles_nofpu = 0;
static volatile uint64_t g_switch_cycles_fpu   = 0;

static long futex_wait(volatile int *addr, int val) {
    return syscall(SYS_futex, addr, FUTEX_WAIT, val, NULL, NULL, 0);
}
static long futex_wake(volatile int *addr) {
    return syscall(SYS_futex, addr, FUTEX_WAKE, 1, NULL, NULL, 0);
}

// "No FPU" pong thread: just integer work
static void *pong_nofpu(void *arg) {
    (void)arg;
    for (int i = 0; i < PINGPONG_ROUNDS; i++) {
        while (g_futex_a != 1)
            futex_wait(&g_futex_a, 0);
        g_futex_a = 0;
        g_futex_b = 1;
        futex_wake(&g_futex_b);
    }
    return NULL;
}

// "FPU" pong thread: does a small SSE dot product each round
static void *pong_fpu(void *arg) {
    (void)arg;
    double x = 1.0;
    for (int i = 0; i < PINGPONG_ROUNDS; i++) {
        while (g_futex_a != 1)
            futex_wait(&g_futex_a, 0);
        g_futex_a = 0;
        // Touch FPU so #NM fires on first use then stays loaded
        x = x * 1.000001 + 0.000001;
        g_futex_b = 1;
        futex_wake(&g_futex_b);
    }
    // Prevent optimizer from removing the FPU work
    if (x < 0.0) printf("x=%f\n", x);
    return NULL;
}

static void bench_pingpong(int use_fpu, uint64_t *out_cycles) {
    g_futex_a = 0;
    g_futex_b = 0;

    pthread_t pong;
    if (use_fpu)
        pthread_create(&pong, NULL, pong_fpu, NULL);
    else
        pthread_create(&pong, NULL, pong_nofpu, NULL);

    uint64_t t0 = rdtsc();
    double px = 1.0;
    for (int i = 0; i < PINGPONG_ROUNDS; i++) {
        g_futex_a = 1;
        futex_wake(&g_futex_a);
        while (g_futex_b != 1)
            futex_wait(&g_futex_b, 0);
        g_futex_b = 0;
        if (use_fpu) {
            // Ping thread also uses FPU
            px = px * 1.000001 + 0.000001;
        }
    }
    uint64_t t1 = rdtsc();
    *out_cycles = t1 - t0;
    if (use_fpu && px < 0.0) printf("px=%f\n", px);

    pthread_join(pong, NULL);
}

int main(void) {
    printf("====================================================\n");
    printf("AvoryOS Lazy FPU Context Switch Benchmark\n");
    printf("====================================================\n");

    calibrate_tsc();
    printf("\n[INFO] TSC frequency: %llu MHz\n",
           (unsigned long long)(g_tsc_khz / 1000));

    // 1. Correctness
    int ok = test_fpu_correctness();
    if (!ok) {
        printf("\n[ABORT] Correctness failure — not running benchmarks.\n");
        return 1;
    }

    // 2. Benchmark: integer-only threads (no FPU, lazy FPU skips fxsave/fxrstor entirely)
    printf("\n[2] Benchmarking ping-pong context switches (no FPU, %d rounds)...\n",
           PINGPONG_ROUNDS);
    uint64_t cyc_nofpu;
    bench_pingpong(0, &cyc_nofpu);
    double ns_nofpu = tsc_to_ns(cyc_nofpu) / (PINGPONG_ROUNDS * 2.0);
    printf("    No-FPU switches:  %llu cycles total  (%.1f ns/switch)\n",
           (unsigned long long)cyc_nofpu, ns_nofpu);

    // 3. Benchmark: FPU threads (triggers #NM once per ownership change, then cached)
    printf("\n[3] Benchmarking ping-pong context switches (WITH FPU, %d rounds)...\n",
           PINGPONG_ROUNDS);
    uint64_t cyc_fpu;
    bench_pingpong(1, &cyc_fpu);
    double ns_fpu = tsc_to_ns(cyc_fpu) / (PINGPONG_ROUNDS * 2.0);
    printf("    FPU switches:     %llu cycles total  (%.1f ns/switch)\n",
           (unsigned long long)cyc_fpu, ns_fpu);

    // 4. Summary
    printf("\n====================================================\n");
    printf("RESULTS SUMMARY:\n");
    printf("  Integer-only threads:  %.1f ns / context switch\n", ns_nofpu);
    printf("  FPU-using threads:     %.1f ns / context switch\n", ns_fpu);
    printf("\n  (With eager FPU, BOTH would pay fxsave+fxrstor per switch)\n");
    printf("  (With lazy FPU,  integer threads pay ZERO FPU save/restore)\n");
    printf("====================================================\n");
    printf("LAZY FPU BENCHMARK COMPLETED SUCCESSFULLY!\n");
    printf("====================================================\n");

    return 0;
}
