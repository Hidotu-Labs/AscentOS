#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>
#include <stdint.h>

#define COUNT 10000
#define SIZE 4096

double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int main() {
    void *ptrs[COUNT];

    printf("--- Mmap Stress Test (%d mappings) ---\n", COUNT);

    double start = get_time_ms();
    for (int i = 0; i < COUNT; i++) {
        ptrs[i] = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptrs[i] == MAP_FAILED) {
            printf("Failed at %d\n", i);
            perror("mmap");
            break;
        }
    }
    double end = get_time_ms();
    printf("Mapping %d pages took: %.3f ms (avg %.3f us/map)\n", 
           COUNT, end - start, ((end - start) * 1000.0) / COUNT);

    printf("Unmapping...\n");
    start = get_time_ms();
    for (int i = 0; i < COUNT; i++) {
        if (ptrs[i] != MAP_FAILED) {
            munmap(ptrs[i], SIZE);
        }
    }
    end = get_time_ms();
    printf("Unmapping %d pages took: %.3f ms (avg %.3f us/unmap)\n", 
           COUNT, end - start, ((end - start) * 1000.0) / COUNT);

    printf("Done.\n");
    return 0;
}
