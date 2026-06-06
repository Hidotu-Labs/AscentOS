#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>
#include <stdint.h>

#define SIZE (512 * 1024 * 1024) // 512 MB

double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int main(int argc, char **argv) {
    size_t size = SIZE;
    if (argc > 1) {
        size = (size_t)atoll(argv[1]) * 1024 * 1024;
    }

    printf("--- Large Mmap Performance Test ---\n");
    printf("Testing with size: %zu MB\n", size / (1024 * 1024));

    double start = get_time_ms();
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    double end = get_time_ms();

    if (ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("mmap took: %.3f ms\n", end - start);

    printf("Touching all pages (forcing faults)...\n");
    start = get_time_ms();
    volatile char *cptr = (volatile char *)ptr;
    for (size_t i = 0; i < size; i += 4096) {
        cptr[i] = 1;
    }
    end = get_time_ms();
    printf("Touch (page faulting) took: %.3f ms\n", end - start);

    printf("Unmapping...\n");
    start = get_time_ms();
    if (munmap(ptr, size) != 0) {
        perror("munmap");
        return 1;
    }
    end = get_time_ms();
    printf("munmap took: %.3f ms\n", end - start);

    printf("Done.\n");
    return 0;
}
