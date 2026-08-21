#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

#define REGION_SIZE (16 * 1024 * 1024) // 16 MB (4096 4KB pages)

int main(void) {
    printf("======================================\n");
    printf("AvoryOS Shared Zero-Page Optimization Test\n");
    printf("======================================\n");

    printf("[TEST] 1. Allocating 16 MB anonymous private mapping...\n");
    void *addr = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }
    printf("[TEST]    mmap returned: %p\n", addr);

    volatile uint8_t *bytes = (volatile uint8_t *)addr;

    printf("[TEST] 2. Reading every page (read fault → shared zero page)...\n");
    for (size_t i = 0; i < REGION_SIZE; i += 4096) {
        if (bytes[i] != 0) {
            printf("[FAIL] Byte at offset 0x%zx was non-zero: %u\n", i, bytes[i]);
            return 1;
        }
    }
    printf("[TEST]    All 4096 pages read 0 successfully!\n");

    printf("[TEST] 3. Writing to every 8th page (CoW break on written pages only)...\n");
    for (size_t i = 0; i < REGION_SIZE; i += 4096 * 8) {
        bytes[i] = (uint8_t)((i / 4096) & 0xFF ? (i / 4096) & 0xFF : 1);
    }
    printf("[TEST]    Written to 512 selected pages.\n");

    printf("[TEST] 4. Verifying written pages and untouched zero pages...\n");
    for (size_t i = 0; i < REGION_SIZE; i += 4096) {
        if (i % (4096 * 8) == 0) {
            uint8_t expected = (uint8_t)((i / 4096) & 0xFF ? (i / 4096) & 0xFF : 1);
            if (bytes[i] != expected) {
                printf("[FAIL] Modified page at offset 0x%zx: got %u, expected %u\n",
                       i, bytes[i], expected);
                return 1;
            }
        } else {
            if (bytes[i] != 0) {
                printf("[FAIL] Untouched page at offset 0x%zx was corrupted: got %u\n",
                       i, bytes[i]);
                return 1;
            }
        }
    }
    printf("[TEST]    Data integrity and page isolation verified!\n");

    printf("[TEST] 5. Unmapping region...\n");
    if (munmap(addr, REGION_SIZE) != 0) {
        perror("munmap failed");
        return 1;
    }

    printf("======================================\n");
    printf("ZERO-PAGE OPTIMIZATION TEST PASSED!\n");
    printf("======================================\n");
    return 0;
}
