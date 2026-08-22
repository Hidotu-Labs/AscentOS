#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define ITERATIONS 50000

static inline uint64_t rdtsc_pure(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(void) {
    printf("====================================================\n");
    printf("AvoryOS Fast uaccess Path Copying Benchmark\n");
    printf("====================================================\n\n");

    // Create a temporary directory structure with short and long paths
    mkdir("/tmp/uaccess_test", 0755);
    int fd = open("/tmp/uaccess_test/short.txt", O_CREAT | O_RDWR, 0644);
    if (fd >= 0) close(fd);

    const char *short_path = "/tmp/uaccess_test/short.txt";
    const char *long_path = "/tmp/uaccess_test/a_very_long_nested_path_component_for_fast_qword_word_at_a_time_string_copying_benchmark_test_file_path_123456789.txt";

    fd = open(long_path, O_CREAT | O_RDWR, 0644);
    if (fd >= 0) close(fd);

    printf("[1] Benchmarking short path access (%d iterations)...\n", ITERATIONS);
    printf("    Path: %s (length: %zu bytes)\n", short_path, strlen(short_path));

    struct stat st;
    uint64_t t0 = rdtsc_pure();
    for (int i = 0; i < ITERATIONS; i++) {
        syscall(SYS_stat, short_path, &st);
    }
    uint64_t t1 = rdtsc_pure();
    uint64_t short_cycles = t1 - t0;
    double short_avg = (double)short_cycles / ITERATIONS;
    printf("    Short path stat(): %lu total cycles (%.1f cycles/lookup)\n\n",
           short_cycles, short_avg);

    printf("[2] Benchmarking long path access (%d iterations)...\n", ITERATIONS);
    printf("    Path: %s (length: %zu bytes)\n", long_path, strlen(long_path));

    t0 = rdtsc_pure();
    for (int i = 0; i < ITERATIONS; i++) {
        syscall(SYS_stat, long_path, &st);
    }
    t1 = rdtsc_pure();
    uint64_t long_cycles = t1 - t0;
    double long_avg = (double)long_cycles / ITERATIONS;
    printf("    Long path stat():  %lu total cycles (%.1f cycles/lookup)\n\n",
           long_cycles, long_avg);

    // Clean up
    unlink(short_path);
    unlink(long_path);
    rmdir("/tmp/uaccess_test");

    printf("====================================================\n");
    printf("uaccess BENCHMARK COMPLETED SUCCESSFULLY!\n");
    printf("====================================================\n");
    return 0;
}
