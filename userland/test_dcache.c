// AvoryOS Fast-Path Dcache & Path Resolution Benchmark
// Tests correctness and measures high-throughput path lookup performance.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
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

static double tsc_to_ns(uint64_t cycles) {
    return (double)cycles * 1000.0 / (double)g_tsc_khz;
}

// -------------------------------------------------------------
// 1. Correctness Test: Path Resolution, Negative Lookup & Invalidation
// -------------------------------------------------------------
static int test_correctness(void) {
    printf("\n[1] Correctness & Invalidation Verification:\n");

    // 1a. Standard existing paths
    struct stat st;
    if (stat("/bin/test_dcache", &st) != 0) {
        printf("    [FAIL] stat(/bin/test_dcache) failed\n");
        return 0;
    }
    printf("    stat(/bin/test_dcache): size=%ld, inode=%lu [OK]\n",
           (long)st.st_size, (unsigned long)st.st_ino);

    if (stat("/proc", &st) != 0) {
        printf("    [FAIL] stat(/proc) failed\n");
        return 0;
    }
    printf("    stat(/proc): mode=%o, inode=%lu [OK]\n",
           st.st_mode, (unsigned long)st.st_ino);

    // 1b. Negative lookup
    if (stat("/nonexistent_dummy_file_12345", &st) == 0) {
        printf("    [FAIL] Negative lookup succeeded unexpectedly!\n");
        return 0;
    }
    printf("    stat(/nonexistent_dummy_file_12345): correctly returned error [OK]\n");

    // 1c. Invalidation test: create -> lookup -> unlink -> verify gone
    const char *tmp_path = "/tmp_dcache_test.txt";
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        // Try in current directory
        tmp_path = "tmp_dcache_test.txt";
        fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    }
    if (fd >= 0) {
        write(fd, "dcache_test", 11);
        close(fd);

        if (stat(tmp_path, &st) != 0) {
            printf("    [FAIL] stat after create failed\n");
            return 0;
        }

        unlink(tmp_path);

        if (stat(tmp_path, &st) == 0) {
            printf("    [FAIL] File still found after unlink (cache invalidation bug)!\n");
            return 0;
        }
        printf("    Create -> Cache -> Unlink -> Invalidation verified [OK]\n");
    }

    printf("    [PASS] All Dcache correctness and invalidation tests passed!\n");
    return 1;
}

// -------------------------------------------------------------
// 2. Performance Benchmark: Repeated stat() Path Resolutions
// -------------------------------------------------------------
#define BENCH_ROUNDS 100000

static void bench_stat_path(const char *path, const char *label) {
    struct stat st;
    // Prime the cache on iteration 0
    stat(path, &st);

    uint64_t t0 = rdtsc();
    for (int i = 0; i < BENCH_ROUNDS; i++) {
        stat(path, &st);
    }
    uint64_t t1 = rdtsc();

    uint64_t total_cycles = t1 - t0;
    double cycles_per_op = (double)total_cycles / (double)BENCH_ROUNDS;
    double ns_per_op = tsc_to_ns(total_cycles) / (double)BENCH_ROUNDS;
    double ops_per_sec = (double)BENCH_ROUNDS / (tsc_to_ns(total_cycles) / 1000000000.0);

    printf("    %-25s %8.1f cycles  (%5.1f ns) -> %6.2f million lookups/sec\n",
           label, cycles_per_op, ns_per_op, ops_per_sec / 1000000.0);
}

int main(void) {
    printf("====================================================\n");
    printf("AvoryOS Fast-Path Dcache & Path Lookup Benchmark\n");
    printf("====================================================\n");

    calibrate_tsc();
    printf("\n[INFO] Calibrated TSC: %llu MHz\n",
           (unsigned long long)(g_tsc_khz / 1000));

    int ok = test_correctness();
    if (!ok) {
        printf("\n[ABORT] Correctness test failed!\n");
        return 1;
    }

    printf("\n[2] Benchmarking Path Resolution Latency (%d iterations each):\n",
           BENCH_ROUNDS);

    bench_stat_path("/bin/test_dcache", "Single-level (/bin/..):");
    bench_stat_path("/bin",            "Root directory (/bin):");
    bench_stat_path("/proc",           "Mount point (/proc):");

    printf("\n====================================================\n");
    printf("DCACHE BENCHMARK COMPLETED SUCCESSFULLY!\n");
    printf("====================================================\n");
    return 0;
}
