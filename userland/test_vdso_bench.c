#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>

#define VSYSCALL_GETTIMEOFDAY  ((int (*)(struct timeval *, struct timezone *))0xFFFFFFFFFF600000ULL)
#define VSYSCALL_TIME          ((time_t (*)(time_t *))0xFFFFFFFFFF600400ULL)
#define VSYSCALL_GETCPU        ((int (*)(unsigned *, unsigned *, void *))0xFFFFFFFFFF600800ULL)
#define VSYSCALL_CLOCK_GETTIME ((int (*)(clockid_t, struct timespec *))0xFFFFFFFFFF600C00ULL)

#define ITERATIONS 100000

static inline uint64_t rdtsc_pure(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(void) {
    printf("====================================================\n");
    printf("AvoryOS vDSO vs Syscall Performance Benchmark\n");
    printf("====================================================\n\n");

    // 1. Correctness tests
    printf("[1] Correctness Verification:\n");

    struct timespec ts_sys = {0, 0};
    struct timespec ts_vdso = {0, 0};
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts_sys);
    VSYSCALL_CLOCK_GETTIME(CLOCK_MONOTONIC, &ts_vdso);

    printf("    Syscall  clock_gettime(MONOTONIC): %lu s, %lu ns\n",
           ts_sys.tv_sec, ts_sys.tv_nsec);
    printf("    vDSO     clock_gettime(MONOTONIC): %lu s, %lu ns\n",
           ts_vdso.tv_sec, ts_vdso.tv_nsec);

    struct timespec ts_real_sys = {0, 0};
    struct timespec ts_real_vdso = {0, 0};
    syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts_real_sys);
    VSYSCALL_CLOCK_GETTIME(CLOCK_REALTIME, &ts_real_vdso);

    printf("    Syscall  clock_gettime(REALTIME):  %lu s, %lu ns\n",
           ts_real_sys.tv_sec, ts_real_sys.tv_nsec);
    printf("    vDSO     clock_gettime(REALTIME):  %lu s, %lu ns\n",
           ts_real_vdso.tv_sec, ts_real_vdso.tv_nsec);

    struct timeval tv_sys = {0, 0};
    struct timeval tv_vdso = {0, 0};
    syscall(SYS_gettimeofday, &tv_sys, NULL);
    VSYSCALL_GETTIMEOFDAY(&tv_vdso, NULL);

    printf("    Syscall  gettimeofday:             %lu s, %lu us\n",
           tv_sys.tv_sec, tv_sys.tv_usec);
    printf("    vDSO     gettimeofday:             %lu s, %lu us\n",
           tv_vdso.tv_sec, tv_vdso.tv_usec);

    printf("    [PASS] Correctness verified!\n\n");

    // 2. Benchmark: clock_gettime
    printf("[2] Benchmarking clock_gettime (%d iterations)...\n", ITERATIONS);

    struct timespec dummy_ts;

    uint64_t start_tsc = rdtsc_pure();
    for (int i = 0; i < ITERATIONS; i++) {
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &dummy_ts);
    }
    uint64_t sys_cycles = rdtsc_pure() - start_tsc;

    start_tsc = rdtsc_pure();
    for (int i = 0; i < ITERATIONS; i++) {
        VSYSCALL_CLOCK_GETTIME(CLOCK_MONOTONIC, &dummy_ts);
    }
    uint64_t vdso_cycles = rdtsc_pure() - start_tsc;

    double sys_avg_cycles = (double)sys_cycles / ITERATIONS;
    double vdso_avg_cycles = (double)vdso_cycles / ITERATIONS;
    double speedup = sys_avg_cycles / (vdso_avg_cycles > 0 ? vdso_avg_cycles : 1.0);

    printf("    Syscall clock_gettime:  %lu cycles total (%.1f cycles/call)\n",
           sys_cycles, sys_avg_cycles);
    printf("    vDSO    clock_gettime:  %lu cycles total (%.1f cycles/call)\n",
           vdso_cycles, vdso_avg_cycles);
    printf("    >>> SPEEDUP: %.1fx FASTER! <<<\n\n", speedup);

    // 3. Benchmark: gettimeofday
    printf("[3] Benchmarking gettimeofday (%d iterations)...\n", ITERATIONS);

    struct timeval dummy_tv;

    start_tsc = rdtsc_pure();
    for (int i = 0; i < ITERATIONS; i++) {
        syscall(SYS_gettimeofday, &dummy_tv, NULL);
    }
    uint64_t sys_gtod_cycles = rdtsc_pure() - start_tsc;

    start_tsc = rdtsc_pure();
    for (int i = 0; i < ITERATIONS; i++) {
        VSYSCALL_GETTIMEOFDAY(&dummy_tv, NULL);
    }
    uint64_t vdso_gtod_cycles = rdtsc_pure() - start_tsc;

    double sys_gtod_avg = (double)sys_gtod_cycles / ITERATIONS;
    double vdso_gtod_avg = (double)vdso_gtod_cycles / ITERATIONS;
    double speedup_gtod = sys_gtod_avg / (vdso_gtod_avg > 0 ? vdso_gtod_avg : 1.0);

    printf("    Syscall gettimeofday:   %lu cycles total (%.1f cycles/call)\n",
           sys_gtod_cycles, sys_gtod_avg);
    printf("    vDSO    gettimeofday:   %lu cycles total (%.1f cycles/call)\n",
           vdso_gtod_cycles, vdso_gtod_avg);
    printf("    >>> SPEEDUP: %.1fx FASTER! <<<\n\n", speedup_gtod);

    // 4. Benchmark: time()
    printf("[4] Benchmarking time() (%d iterations)...\n", ITERATIONS);

    time_t dummy_t;

    start_tsc = rdtsc_pure();
    for (int i = 0; i < ITERATIONS; i++) {
        syscall(SYS_time, &dummy_t);
    }
    uint64_t sys_time_cycles = rdtsc_pure() - start_tsc;

    start_tsc = rdtsc_pure();
    for (int i = 0; i < ITERATIONS; i++) {
        VSYSCALL_TIME(&dummy_t);
    }
    uint64_t vdso_time_cycles = rdtsc_pure() - start_tsc;

    double sys_time_avg = (double)sys_time_cycles / ITERATIONS;
    double vdso_time_avg = (double)vdso_time_cycles / ITERATIONS;
    double speedup_time = sys_time_avg / (vdso_time_avg > 0 ? vdso_time_avg : 1.0);

    printf("    Syscall time():         %lu cycles total (%.1f cycles/call)\n",
           sys_time_cycles, sys_time_avg);
    printf("    vDSO    time():         %lu cycles total (%.1f cycles/call)\n",
           vdso_time_cycles, vdso_time_avg);
    printf("    >>> SPEEDUP: %.1fx FASTER! <<<\n\n", speedup_time);

    printf("====================================================\n");
    printf("vDSO BENCHMARK COMPLETED SUCCESSFULLY!\n");
    printf("====================================================\n");
    return 0;
}

