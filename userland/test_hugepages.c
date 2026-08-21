#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

#define REGION_SIZE (4 * 1024 * 1024) // 4 MB = 2 huge pages

static void test_madvise_hugepage(void) {
    printf("[TEST] 1. Testing madvise(MADV_HUGEPAGE)...\n");
    void *addr = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }

    int ret = madvise(addr, REGION_SIZE, MADV_HUGEPAGE);
    if (ret != 0) {
        perror("madvise MADV_HUGEPAGE failed");
        exit(1);
    }
    printf("[TEST]    madvise returned success (%d)\n", ret);

    // Touch every page (4KB stride) to trigger huge page demand faulting
    volatile uint8_t *ptr = (volatile uint8_t *)addr;
    for (size_t i = 0; i < REGION_SIZE; i += 4096) {
        ptr[i] = (uint8_t)(i / 4096 + 1);
    }

    // Verify all written pages
    for (size_t i = 0; i < REGION_SIZE; i += 4096) {
        if (ptr[i] != (uint8_t)(i / 4096 + 1)) {
            printf("[FAIL] Data mismatch at offset 0x%zx: got %u, expected %u\n",
                   i, ptr[i], (uint8_t)(i / 4096 + 1));
            exit(1);
        }
    }
    printf("[TEST]    Data integrity verified across 4 MB range.\n");

    // Test MADV_NOHUGEPAGE
    ret = madvise(addr, REGION_SIZE, MADV_NOHUGEPAGE);
    if (ret != 0) {
        perror("madvise MADV_NOHUGEPAGE failed");
        exit(1);
    }
    printf("[TEST]    madvise(MADV_NOHUGEPAGE) returned success (%d)\n", ret);

    munmap(addr, REGION_SIZE);
    printf("[TEST] 1. PASSED\n\n");
}

static void test_map_hugetLB(void) {
    printf("[TEST] 2. Testing mmap(MAP_HUGETLB)...\n");
    void *addr = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap with MAP_HUGETLB failed");
        exit(1);
    }

    volatile uint32_t *p32 = (volatile uint32_t *)addr;
    size_t words = REGION_SIZE / sizeof(uint32_t);

    for (size_t i = 0; i < words; i += 1024) {
        p32[i] = (uint32_t)(0xDEAD0000 + i);
    }

    for (size_t i = 0; i < words; i += 1024) {
        if (p32[i] != (uint32_t)(0xDEAD0000 + i)) {
            printf("[FAIL] MAP_HUGETLB Data mismatch at word %zu: got 0x%x, expected 0x%x\n",
                   i, p32[i], (uint32_t)(0xDEAD0000 + i));
            exit(1);
        }
    }
    printf("[TEST]    MAP_HUGETLB data integrity verified.\n");

    munmap(addr, REGION_SIZE);
    printf("[TEST] 2. PASSED\n\n");
}

int main(void) {
    printf("======================================\n");
    printf("AvoryOS Transparent Huge Pages (THP) Test\n");
    printf("======================================\n");

    test_madvise_hugepage();
    test_map_hugetLB();

    printf("======================================\n");
    printf("ALL HUGE PAGE TESTS PASSED!\n");
    printf("======================================\n");
    return 0;
}
