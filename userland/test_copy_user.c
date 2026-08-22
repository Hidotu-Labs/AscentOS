#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <stdint.h>
#include <assert.h>

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

int main(void) {
    printf("====================================================\n");
    printf("AvoryOS Usercopy & Exception Fixup Table Test Suite\n");
    printf("====================================================\n\n");

    int pipefds[2];
    if (pipe(pipefds) != 0) {
        perror("pipe failed");
        return 1;
    }

    // -------------------------------------------------------------------------
    // Test 1: Valid I/O Integrity Test
    // -------------------------------------------------------------------------
    printf("[TEST 1] Valid Buffer I/O Integrity (1 MB streaming)...\n");
    size_t chunk_size = 64 * 1024;
    size_t total_bytes = 1024 * 1024;
    uint8_t *wbuf = malloc(chunk_size);
    uint8_t *rbuf = malloc(chunk_size);

    for (size_t i = 0; i < chunk_size; i++) {
        wbuf[i] = (uint8_t)(i ^ (i >> 8));
    }

    size_t transferred = 0;
    while (transferred < total_bytes) {
        ssize_t w = write(pipefds[1], wbuf, chunk_size);
        if (w <= 0) {
            printf("[FAIL] Write returned %zd (errno=%d: %s)\n", w, errno, strerror(errno));
            return 1;
        }
        ssize_t r = read(pipefds[0], rbuf, (size_t)w);
        if (r != w) {
            printf("[FAIL] Read mismatch: expected %zd, got %zd\n", w, r);
            return 1;
        }
        if (memcmp(wbuf, rbuf, (size_t)r) != 0) {
            printf("[FAIL] Data corruption detected in transferred stream!\n");
            return 1;
        }
        transferred += (size_t)w;
    }
    printf("[PASS]   1 MB stream transferred and verified bit-for-bit!\n\n");

    // -------------------------------------------------------------------------
    // Test 2: Invalid User Pointer Resilience (No kernel panic, clean -EFAULT)
    // -------------------------------------------------------------------------
    printf("[TEST 2] Invalid Pointer Safety & Fixup Table Recovery...\n");

    // 2.1 NULL Pointer Write
    errno = 0;
    ssize_t ret = write(pipefds[1], NULL, 64);
    if (ret != -1 || errno != EFAULT) {
        printf("[FAIL] write(NULL) returned %zd (errno=%d, expected EFAULT=%d)\n",
               ret, errno, EFAULT);
        return 1;
    }
    printf("[PASS]   write(NULL) cleanly returned -EFAULT (%d)\n", errno);

    // 2.2 NULL Pointer Read (seed pipe with 4 bytes so read doesn't block on empty pipe)
    write(pipefds[1], "TEST", 4);
    errno = 0;
    ret = read(pipefds[0], NULL, 64);
    if (ret != -1 || errno != EFAULT) {
        printf("[FAIL] read(NULL) returned %zd (errno=%d, expected EFAULT=%d)\n",
               ret, errno, EFAULT);
        return 1;
    }
    printf("[PASS]   read(NULL) cleanly returned -EFAULT (%d)\n", errno);

    // 2.3 Unmapped Wild Address
    void *bad_addr = (void *)0xdeadbeef0000ULL;
    errno = 0;
    ret = write(pipefds[1], bad_addr, 128);
    if (ret != -1 || errno != EFAULT) {
        printf("[FAIL] write(0xdeadbeef) returned %zd (errno=%d)\n", ret, errno);
        return 1;
    }
    printf("[PASS]   write(unmapped) caught by fixup table, returned -EFAULT\n");

    errno = 0;
    ret = read(pipefds[0], bad_addr, 64);
    if (ret != -1 || errno != EFAULT) {
        printf("[FAIL] read(unmapped) returned %zd (errno=%d)\n", ret, errno);
        return 1;
    }
    printf("[PASS]   read(unmapped) caught by fixup table, returned -EFAULT\n");

    // Drain the test bytes
    char drain[16];
    read(pipefds[0], drain, 4);

    // 2.4 Kernel Space Pointer (Security Boundary)
    void *kernel_addr = (void *)0xffffffff80000000ULL;
    errno = 0;
    ret = write(pipefds[1], kernel_addr, 64);
    if (ret != -1 || errno != EFAULT) {
        printf("[FAIL] write(kernel_addr) returned %zd (errno=%d)\n", ret, errno);
        return 1;
    }
    printf("[PASS]   write(kernel_addr) rejected as out of user range (-EFAULT)\n");

    // 2.5 Poll with Bad Pointer
    errno = 0;
    int p_ret = poll((struct pollfd *)bad_addr, 2, 0);
    if (p_ret != -1 || errno != EFAULT) {
        printf("[FAIL] poll(bad_addr) returned %d (errno=%d)\n", p_ret, errno);
        return 1;
    }
    printf("[PASS]   poll(bad_addr) returned -EFAULT\n\n");

    // -------------------------------------------------------------------------
    // Test 3: Page Boundary Crossing Test
    // -------------------------------------------------------------------------
    printf("[TEST 3] Page Boundary Crossing Test...\n");
    void *two_pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (two_pages == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

    memset(two_pages, 'A', 4096);
    // Unmap second page
    munmap((uint8_t *)two_pages + 4096, 4096);

    // Buffer starts 16 bytes before page boundary, crossing into unmapped page
    uint8_t *crossing_buf = (uint8_t *)two_pages + 4096 - 16;
    errno = 0;
    ret = write(pipefds[1], crossing_buf, 64);
    // Should either write 16 bytes (partial copy before fault) or return EFAULT
    if (ret > 0) {
        printf("[PASS]   Partial copy handled gracefully (%zd bytes copied before boundary fault)\n", ret);
        read(pipefds[0], drain, (size_t)ret);
    } else if (ret == -1 && errno == EFAULT) {
        printf("[PASS]   Fault at boundary caught by fixup table, returned -EFAULT\n");
    } else {
        printf("[FAIL] Unexpected return value %zd (errno=%d)\n", ret, errno);
        return 1;
    }

    munmap(two_pages, 4096);
    printf("\n");

    // -------------------------------------------------------------------------
    // Test 4: Fast Path Latency & Throughput Benchmark
    // -------------------------------------------------------------------------
    printf("[TEST 4] Performance Benchmark (50,000 Syscall Roundtrips)...\n");
    int iterations = 50000;
    uint64_t msg = 0x123456789abcdef0ULL;
    uint64_t rec = 0;

    double t_start = get_time_sec();
    for (int i = 0; i < iterations; i++) {
        if (write(pipefds[1], &msg, sizeof(msg)) != sizeof(msg)) {
            perror("bench write");
            return 1;
        }
        if (read(pipefds[0], &rec, sizeof(rec)) != sizeof(rec)) {
            perror("bench read");
            return 1;
        }
    }
    double t_end = get_time_sec();
    double total_time = t_end - t_start;
    double ops_per_sec = (double)(iterations * 2) / total_time;
    double us_per_call = (total_time / (double)(iterations * 2)) * 1000000.0;

    printf("[BENCH]  Total Time:       %.4f seconds\n", total_time);
    printf("[BENCH]  Syscalls/sec:     %.0f ops/sec\n", ops_per_sec);
    printf("[BENCH]  Latency/Syscall:  %.3f microseconds\n\n", us_per_call);

    close(pipefds[0]);
    close(pipefds[1]);
    free(wbuf);
    free(rbuf);

    printf("====================================================\n");
    printf("ALL TESTS PASSED SUCCESSFULLY!\n");
    printf("====================================================\n");
    return 0;
}
