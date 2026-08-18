/*
 * test_vfs_path_bench.c - VFS Path Resolution & Dentry Cache Benchmark Suite
 *
 * Measures performance of deep path lookup, positive and negative dentry resolution,
 * multi-core concurrent scaling, and invalidation overhead.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>

#define BENCH_ROOT "/tmp/bench_vfs_test"
#define NUM_FONTS 32
#define DEFAULT_ITERATIONS 10000

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void create_directory_p(const char *path) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static void setup_benchmark_environment(void) {
    char dirpath[256];
    char filepath[256];

    snprintf(dirpath, sizeof(dirpath), "%s/usr/share/fonts/truetype/noto", BENCH_ROOT);
    create_directory_p(dirpath);

    snprintf(dirpath, sizeof(dirpath), "%s/usr/lib/x86_64-linux-gnu", BENCH_ROOT);
    create_directory_p(dirpath);

    // Create mock font files
    for (int i = 0; i < NUM_FONTS; i++) {
        snprintf(filepath, sizeof(filepath), "%s/usr/share/fonts/truetype/noto/NotoSans_%02d.ttf",
                 BENCH_ROOT, i);
        int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "MOCK_FONT_DATA", 14);
            close(fd);
        }
    }

    // Create mock library files
    for (int i = 0; i < 16; i++) {
        snprintf(filepath, sizeof(filepath), "%s/usr/lib/x86_64-linux-gnu/libwebkit_%02d.so",
                 BENCH_ROOT, i);
        int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "MOCK_ELF_LIB", 12);
            close(fd);
        }
    }
}

static void cleanup_benchmark_environment(void) {
    char filepath[256];
    for (int i = 0; i < NUM_FONTS; i++) {
        snprintf(filepath, sizeof(filepath), "%s/usr/share/fonts/truetype/noto/NotoSans_%02d.ttf",
                 BENCH_ROOT, i);
        unlink(filepath);
    }
    for (int i = 0; i < 16; i++) {
        snprintf(filepath, sizeof(filepath), "%s/usr/lib/x86_64-linux-gnu/libwebkit_%02d.so",
                 BENCH_ROOT, i);
        unlink(filepath);
    }
    snprintf(filepath, sizeof(filepath), "%s/usr/share/fonts/truetype/noto", BENCH_ROOT);
    rmdir(filepath);
    snprintf(filepath, sizeof(filepath), "%s/usr/share/fonts/truetype", BENCH_ROOT);
    rmdir(filepath);
    snprintf(filepath, sizeof(filepath), "%s/usr/share/fonts", BENCH_ROOT);
    rmdir(filepath);
    snprintf(filepath, sizeof(filepath), "%s/usr/share", BENCH_ROOT);
    rmdir(filepath);
    snprintf(filepath, sizeof(filepath), "%s/usr/lib/x86_64-linux-gnu", BENCH_ROOT);
    rmdir(filepath);
    snprintf(filepath, sizeof(filepath), "%s/usr/lib", BENCH_ROOT);
    rmdir(filepath);
    snprintf(filepath, sizeof(filepath), "%s/usr", BENCH_ROOT);
    rmdir(filepath);
    rmdir(BENCH_ROOT);
}

// -----------------------------------------------------------------------------
// Benchmark 1: Positive Deep Path Resolution (openat + close)
// -----------------------------------------------------------------------------
static void bench_positive_lookups(int iterations) {
    char paths[NUM_FONTS][256];
    for (int i = 0; i < NUM_FONTS; i++) {
        snprintf(paths[i], sizeof(paths[i]),
                 "%s/usr/share/fonts/truetype/noto/NotoSans_%02d.ttf",
                 BENCH_ROOT, i);
    }

    // Warm up cache
    for (int i = 0; i < NUM_FONTS; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd >= 0) close(fd);
    }

    uint64_t start = get_time_ns();
    int success_count = 0;

    for (int i = 0; i < iterations; i++) {
        const char *p = paths[i % NUM_FONTS];
        int fd = open(p, O_RDONLY);
        if (fd >= 0) {
            success_count++;
            close(fd);
        }
    }

    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    double duration_ms = (double)duration_ns / 1000000.0;
    double ns_per_op = (double)duration_ns / (double)iterations;
    double ops_per_sec = ((double)iterations / (double)duration_ns) * 1000000000.0;

    printf("[TEST 1] Positive Deep Path Lookup (open/close):\n");
    printf("         Iterations: %d | Success: %d\n", iterations, success_count);
    printf("         Total Time: %.2f ms | Avg Latency: %.1f ns/lookup | Throughput: %.0f ops/sec\n\n",
           duration_ms, ns_per_op, ops_per_sec);
}

// -----------------------------------------------------------------------------
// Benchmark 2: Negative Deep Path Resolution (ENOENT)
// -----------------------------------------------------------------------------
static void bench_negative_lookups(int iterations) {
    char paths[16][256];
    for (int i = 0; i < 16; i++) {
        snprintf(paths[i], sizeof(paths[i]),
                 "%s/usr/share/fonts/truetype/noto/NonExistentFont_%02d.ttf",
                 BENCH_ROOT, i);
    }

    // Warm up negative cache
    for (int i = 0; i < 16; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd >= 0) close(fd);
    }

    uint64_t start = get_time_ns();
    int enoent_count = 0;

    for (int i = 0; i < iterations; i++) {
        const char *p = paths[i % 16];
        int fd = open(p, O_RDONLY);
        if (fd < 0 && errno == ENOENT) {
            enoent_count++;
        } else if (fd >= 0) {
            close(fd);
        }
    }

    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    double duration_ms = (double)duration_ns / 1000000.0;
    double ns_per_op = (double)duration_ns / (double)iterations;
    double ops_per_sec = ((double)iterations / (double)duration_ns) * 1000000000.0;

    printf("[TEST 2] Negative Deep Path Lookup (ENOENT probing):\n");
    printf("         Iterations: %d | ENOENT: %d\n", iterations, enoent_count);
    printf("         Total Time: %.2f ms | Avg Latency: %.1f ns/lookup | Throughput: %.0f ops/sec\n\n",
           duration_ms, ns_per_op, ops_per_sec);
}

// -----------------------------------------------------------------------------
// Benchmark 3: Stat/Access Resolution Speed
// -----------------------------------------------------------------------------
static void bench_stat_lookups(int iterations) {
    char paths[NUM_FONTS][256];
    for (int i = 0; i < NUM_FONTS; i++) {
        snprintf(paths[i], sizeof(paths[i]),
                 "%s/usr/share/fonts/truetype/noto/NotoSans_%02d.ttf",
                 BENCH_ROOT, i);
    }

    struct stat st;
    uint64_t start = get_time_ns();
    int success_count = 0;

    for (int i = 0; i < iterations; i++) {
        const char *p = paths[i % NUM_FONTS];
        if (stat(p, &st) == 0) {
            success_count++;
        }
    }

    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    double duration_ms = (double)duration_ns / 1000000.0;
    double ns_per_op = (double)duration_ns / (double)iterations;
    double ops_per_sec = ((double)iterations / (double)duration_ns) * 1000000000.0;

    printf("[TEST 3] Pure Path Stat Resolution (stat() syscall):\n");
    printf("         Iterations: %d | Success: %d\n", iterations, success_count);
    printf("         Total Time: %.2f ms | Avg Latency: %.1f ns/stat | Throughput: %.0f ops/sec\n\n",
           duration_ms, ns_per_op, ops_per_sec);
}

// -----------------------------------------------------------------------------
// Benchmark 4: Multi-Worker Concurrent Path Scaling (Lock Contention)
// -----------------------------------------------------------------------------
static void bench_concurrent_lookups(int num_workers, int iters_per_worker) {
    char paths[NUM_FONTS][256];
    for (int i = 0; i < NUM_FONTS; i++) {
        snprintf(paths[i], sizeof(paths[i]),
                 "%s/usr/share/fonts/truetype/noto/NotoSans_%02d.ttf",
                 BENCH_ROOT, i);
    }

    pid_t pids[16];
    uint64_t start = get_time_ns();

    for (int w = 0; w < num_workers; w++) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            for (int i = 0; i < iters_per_worker; i++) {
                const char *p = paths[(i + w) % NUM_FONTS];
                int fd = open(p, O_RDONLY);
                if (fd >= 0) close(fd);
            }
            _exit(0);
        }
        pids[w] = pid;
    }

    // Wait for all workers
    for (int w = 0; w < num_workers; w++) {
        int status;
        waitpid(pids[w], &status, 0);
    }

    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    int total_iters = num_workers * iters_per_worker;
    double duration_ms = (double)duration_ns / 1000000.0;
    double aggregate_ops_per_sec = ((double)total_iters / (double)duration_ns) * 1000000000.0;

    printf("[TEST 4] Concurrent Multi-Worker Resolution (%d Workers, %d ops/worker):\n",
           num_workers, iters_per_worker);
    printf("         Total Ops: %d in %.2f ms | Aggregate Throughput: %.0f ops/sec\n\n",
           total_iters, duration_ms, aggregate_ops_per_sec);
}

// -----------------------------------------------------------------------------
// Benchmark 5: Resolution with Concurrent File Invalidation
// -----------------------------------------------------------------------------
static void bench_invalidation_mix(int iterations) {
    char paths[NUM_FONTS][256];
    for (int i = 0; i < NUM_FONTS; i++) {
        snprintf(paths[i], sizeof(paths[i]),
                 "%s/usr/share/fonts/truetype/noto/NotoSans_%02d.ttf",
                 BENCH_ROOT, i);
    }

    char inv_path[256];
    snprintf(inv_path, sizeof(inv_path), "%s/usr/share/fonts/truetype/noto/tmp_dyn_file.ttf", BENCH_ROOT);

    pid_t inv_pid = fork();
    if (inv_pid == 0) {
        // Invalidation worker: rapidly create and unlink files
        for (int i = 0; i < iterations / 4; i++) {
            int fd = open(inv_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) {
                close(fd);
                unlink(inv_path);
            }
        }
        _exit(0);
    }

    uint64_t start = get_time_ns();
    int success_count = 0;

    for (int i = 0; i < iterations; i++) {
        const char *p = paths[i % NUM_FONTS];
        int fd = open(p, O_RDONLY);
        if (fd >= 0) {
            success_count++;
            close(fd);
        }
    }

    int status;
    waitpid(inv_pid, &status, 0);

    uint64_t end = get_time_ns();
    uint64_t duration_ns = end - start;
    double duration_ms = (double)duration_ns / 1000000.0;
    double ns_per_op = (double)duration_ns / (double)iterations;
    double ops_per_sec = ((double)iterations / (double)duration_ns) * 1000000000.0;

    printf("[TEST 5] Path Lookup Under Concurrent Invalidation Stress:\n");
    printf("         Iterations: %d | Success: %d\n", iterations, success_count);
    printf("         Total Time: %.2f ms | Avg Latency: %.1f ns/lookup | Throughput: %.0f ops/sec\n\n",
           duration_ms, ns_per_op, ops_per_sec);
}

int main(int argc, char **argv) {
    int iterations = DEFAULT_ITERATIONS;
    if (argc > 1) {
        iterations = atoi(argv[1]);
        if (iterations <= 0) iterations = DEFAULT_ITERATIONS;
    }

    printf("=======================================================================\n");
    printf("     AvoryOS VFS Path Resolution & Dentry Benchmark Suite              \n");
    printf("     Testing deep font & library paths (Base Iterations: %d)          \n", iterations);
    printf("=======================================================================\n\n");

    setup_benchmark_environment();

    bench_positive_lookups(iterations);
    bench_negative_lookups(iterations);
    bench_stat_lookups(iterations);
    bench_concurrent_lookups(2, iterations / 2);
    bench_concurrent_lookups(4, iterations / 4);
    bench_invalidation_mix(iterations);

    cleanup_benchmark_environment();

    printf("=======================================================================\n");
    printf("Benchmark suite completed successfully.\n");
    printf("=======================================================================\n");
    return 0;
}
