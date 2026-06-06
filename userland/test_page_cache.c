#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#define SIZE (64 * 1024 * 1024) // 64 MB

double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int main() {
    int fd = open("stress_test.data", O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // --- Phase 0: Write Performance ---
    printf("--- Phase 0: Write Performance ---\n");
    char *buf = malloc(SIZE);
    if (!buf) return 1;
    memset(buf, 0xAA, SIZE);

    double write_start = get_time_ms();
    if (write(fd, buf, SIZE) != SIZE) {
        perror("write");
        return 1;
    }
    double write_end = get_time_ms();
    double write_ms = write_end - write_start;
    printf("Writing %d MB took: %.3f ms (%.1f MB/s)\n",
           SIZE / (1024 * 1024), write_ms,
           write_ms > 0 ? (SIZE / (1024.0 * 1024.0)) / (write_ms / 1000.0) : 0);
    free(buf);

    // --- Phase 1: Cold Cache (First mmap Access) ---
    printf("\n--- Phase 1: Cold Cache (First Access) ---\n");
    void *ptr1 = mmap(NULL, SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (ptr1 == MAP_FAILED) { perror("mmap"); return 1; }

    double start = get_time_ms();
    volatile char *cptr1 = (volatile char *)ptr1;
    uint64_t sum = 0;
    for (size_t i = 0; i < SIZE; i += 4096) {
        sum += cptr1[i];
    }
    double end = get_time_ms();
    double cold_ms = end - start;
    printf("Cold mmap faults took: %.3f ms (sum: %lu)\n", cold_ms, sum);

    // --- Phase 2: Warm Cache (Second Mapping) ---
    printf("\n--- Phase 2: Warm Cache (Second Mapping) ---\n");
    void *ptr2 = mmap(NULL, SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (ptr2 == MAP_FAILED) { perror("mmap"); return 1; }

    start = get_time_ms();
    volatile char *cptr2 = (volatile char *)ptr2;
    sum = 0;
    for (size_t i = 0; i < SIZE; i += 4096) {
        sum += cptr2[i];
    }
    end = get_time_ms();
    double warm_ms = end - start;
    printf("Warm mmap faults took: %.3f ms (sum: %lu)\n", warm_ms, sum);

    // --- Results ---
    printf("\n--- Results ---\n");
    printf("Write:     %.3f ms\n", write_ms);
    printf("Cold mmap: %.3f ms\n", cold_ms);
    printf("Warm mmap: %.3f ms\n", warm_ms);
    if (warm_ms > 0 && cold_ms > 0) {
        printf("Cache speedup (cold/warm): %.1fx\n", cold_ms / warm_ms);
    }

    munmap(ptr1, SIZE);
    munmap(ptr2, SIZE);
    close(fd);
    unlink("stress_test.data");

    return 0;
}
