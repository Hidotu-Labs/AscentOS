// ── Socket Stress Test for Phase 2: Socket Families & Protocols ───────────────────
// Tests: family registration, protocol validation, type validation

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

// Simple assert macro that prints and tracks failures
static int test_passed = 0;
static int test_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d): %s\n", msg, __LINE__, strerror(errno)); \
        test_failed++; \
        return -1; \
    } \
    test_passed++; \
} while(0)

#define TEST_START(name) printf("\n=== Test: %s ===\n", name)
#define TEST_END() printf("  Passed: %d, Failed: %d\n", test_passed, test_failed)

// ── Test 1: Valid Family/Type Combinations ─────────────────────────────────────
static int test_valid_combinations(void) {
    TEST_START("Valid Family/Type Combinations");
    
    // AF_UNIX + SOCK_STREAM
    int fd1 = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(fd1 >= 0, "AF_UNIX + SOCK_STREAM should succeed");
    printf("  AF_UNIX + SOCK_STREAM: fd=%d\n", fd1);
    close(fd1);
    
    // AF_UNIX + SOCK_DGRAM
    int fd2 = socket(AF_UNIX, SOCK_DGRAM, 0);
    TEST_ASSERT(fd2 >= 0, "AF_UNIX + SOCK_DGRAM should succeed");
    printf("  AF_UNIX + SOCK_DGRAM: fd=%d\n", fd2);
    close(fd2);
    
    // AF_UNIX + SOCK_SEQPACKET
    int fd3 = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    TEST_ASSERT(fd3 >= 0, "AF_UNIX + SOCK_SEQPACKET should succeed");
    printf("  AF_UNIX + SOCK_SEQPACKET: fd=%d\n", fd3);
    close(fd3);
    
    TEST_END();
    return 0;
}

// ── Test 2: Invalid Protocol ───────────────────────────────────────────────────
static int test_invalid_protocol(void) {
    TEST_START("Invalid Protocol");
    
    // AF_UNIX requires protocol=0
    int fd = socket(AF_UNIX, SOCK_STREAM, 1);
    TEST_ASSERT(fd < 0, "Protocol=1 should fail for AF_UNIX");
    TEST_ASSERT(errno == EPROTONOSUPPORT, "Should return EPROTONOSUPPORT");
    printf("  Protocol=1 correctly rejected (errno=%d EPROTONOSUPPORT=%d)\n", 
           errno, EPROTONOSUPPORT);
    
    // Protocol=99 should also fail
    fd = socket(AF_UNIX, SOCK_DGRAM, 99);
    TEST_ASSERT(fd < 0, "Protocol=99 should fail for AF_UNIX");
    printf("  Protocol=99 correctly rejected\n");
    
    TEST_END();
    return 0;
}

// ── Test 3: Invalid Family ─────────────────────────────────────────────────────
static int test_invalid_family(void) {
    TEST_START("Invalid Family");
    
    // Family 99 doesn't exist
    int fd = socket(99, SOCK_STREAM, 0);
    TEST_ASSERT(fd < 0, "Invalid family should fail");
    TEST_ASSERT(errno == EAFNOSUPPORT, "Should return EAFNOSUPPORT");
    printf("  Family=99 correctly rejected (errno=%d EAFNOSUPPORT=%d)\n",
           errno, EAFNOSUPPORT);
    
    // Internet families are registered by the network stack.
    fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(fd >= 0, "AF_INET should be implemented");
    close(fd);

    fd = socket(AF_INET6, SOCK_STREAM, 0);
    TEST_ASSERT(fd >= 0, "AF_INET6 should be implemented");
    close(fd);
    
    TEST_END();
    return 0;
}

// ── Test 4: Invalid Type ───────────────────────────────────────────────────────
static int test_invalid_type(void) {
    TEST_START("Invalid Type");
    
    // Type 999 doesn't exist
    int fd = socket(AF_UNIX, 999, 0);
    TEST_ASSERT(fd < 0, "Invalid type should fail");
    TEST_ASSERT(errno == EPROTONOSUPPORT || errno == EINVAL, 
                "Should return EPROTONOSUPPORT or EINVAL");
    printf("  Type=999 correctly rejected (errno=%d)\n", errno);
    
    // Type 0 is invalid
    fd = socket(AF_UNIX, 0, 0);
    TEST_ASSERT(fd < 0, "Type=0 should fail");
    printf("  Type=0 correctly rejected\n");
    
    TEST_END();
    return 0;
}

// ── Test 5: Mixed Type Creation ────────────────────────────────────────────────
// Create many sockets of different types
static int test_mixed_type_creation(void) {
    TEST_START("Mixed Type Creation (1000 each)");
    
    #define NUM_SOCKETS 1000
    int streams[NUM_SOCKETS];
    int dgrams[NUM_SOCKETS];
    int seqpackets[NUM_SOCKETS];
    int s_count = 0, d_count = 0, sq_count = 0;
    
    // Create all types
    for (int i = 0; i < NUM_SOCKETS; i++) {
        streams[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        if (streams[i] >= 0) s_count++;
        
        dgrams[i] = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (dgrams[i] >= 0) d_count++;
        
        seqpackets[i] = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (seqpackets[i] >= 0) sq_count++;
        
        if (i % 200 == 0 && i > 0) {
            printf("  Progress: %d/%d\n", i, NUM_SOCKETS);
        }
    }
    
    printf("  Created: %d streams, %d dgrams, %d seqpackets\n",
           s_count, d_count, sq_count);
    
    TEST_ASSERT(s_count > 0, "Should create some stream sockets");
    TEST_ASSERT(d_count > 0, "Should create some dgram sockets");
    TEST_ASSERT(sq_count > 0, "Should create some seqpacket sockets");
    
    // Close all
    for (int i = 0; i < NUM_SOCKETS; i++) {
        if (streams[i] >= 0) close(streams[i]);
        if (dgrams[i] >= 0) close(dgrams[i]);
        if (seqpackets[i] >= 0) close(seqpackets[i]);
    }
    
    printf("  Closed all sockets\n");
    TEST_END();
    return 0;
}

// ── Test 6: Socketpair with Different Types ─────────────────────────────────────
static int test_socketpair_types(void) {
    TEST_START("Socketpair Types");
    
    int sv[2];
    
    // SOCK_STREAM socketpair
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "SOCK_STREAM socketpair should succeed");
    printf("  SOCK_STREAM socketpair: [%d, %d]\n", sv[0], sv[1]);
    close(sv[0]);
    close(sv[1]);
    
    // SOCK_DGRAM socketpair
    ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    TEST_ASSERT(ret == 0, "SOCK_DGRAM socketpair should succeed");
    printf("  SOCK_DGRAM socketpair: [%d, %d]\n", sv[0], sv[1]);
    close(sv[0]);
    close(sv[1]);
    
    // SOCK_SEQPACKET socketpair
    ret = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    TEST_ASSERT(ret == 0, "SOCK_SEQPACKET socketpair should succeed");
    printf("  SOCK_SEQPACKET socketpair: [%d, %d]\n", sv[0], sv[1]);
    close(sv[0]);
    close(sv[1]);
    
    // Invalid type for socketpair
    ret = socketpair(AF_UNIX, 999, 0, sv);
    TEST_ASSERT(ret < 0, "Invalid type socketpair should fail");
    printf("  Invalid type socketpair correctly rejected\n");
    
    TEST_END();
    return 0;
}

// ── Test 7: Socket Options for Different Types ──────────────────────────────────
static int test_socket_options_by_type(void) {
    TEST_START("Socket Options by Type");
    
    int types[] = {SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET};
    const char *type_names[] = {"SOCK_STREAM", "SOCK_DGRAM", "SOCK_SEQPACKET"};
    
    for (int t = 0; t < 3; t++) {
        int fd = socket(AF_UNIX, types[t], 0);
        TEST_ASSERT(fd >= 0, "Socket creation should succeed");
        
        // Get SO_TYPE
        int so_type = 0;
        socklen_t len = sizeof(so_type);
        int ret = getsockopt(fd, SOL_SOCKET, SO_TYPE, &so_type, &len);
        TEST_ASSERT(ret == 0, "getsockopt SO_TYPE should succeed");
        TEST_ASSERT(so_type == types[t], "SO_TYPE should match created type");
        printf("  %s: SO_TYPE=%d (expected %d)\n", type_names[t], so_type, types[t]);
        
        // Get SO_DOMAIN
        int domain = 0;
        len = sizeof(domain);
        ret = getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len);
        TEST_ASSERT(ret == 0, "getsockopt SO_DOMAIN should succeed");
        TEST_ASSERT(domain == AF_UNIX, "SO_DOMAIN should be AF_UNIX");
        
        close(fd);
    }
    
    TEST_END();
    return 0;
}

// ── Test 8: Rapid Create/Close Different Types ─────────────────────────────────
static int test_rapid_create_close_types(void) {
    TEST_START("Rapid Create/Close Different Types");
    
    #define RAPID_CYCLES 10000
    
    for (int i = 0; i < RAPID_CYCLES; i++) {
        int fd1 = socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(fd1 >= 0, "SOCK_STREAM creation should succeed");
        close(fd1);
        
        int fd2 = socket(AF_UNIX, SOCK_DGRAM, 0);
        TEST_ASSERT(fd2 >= 0, "SOCK_DGRAM creation should succeed");
        close(fd2);
        
        int fd3 = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        TEST_ASSERT(fd3 >= 0, "SOCK_SEQPACKET creation should succeed");
        close(fd3);
        
        if (i % 2000 == 0 && i > 0) {
            printf("  Progress: %d/%d cycles\n", i, RAPID_CYCLES);
        }
    }
    
    printf("  Completed %d rapid create/close cycles (3 sockets each)\n", RAPID_CYCLES);
    TEST_END();
    return 0;
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║   Socket Stress Test - Phase 2: Socket Families & Protocols ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    int total_passed = 0;
    int total_failed = 0;
    
    // Run all tests
    test_valid_combinations();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_invalid_protocol();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_invalid_family();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_invalid_type();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_mixed_type_creation();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_socketpair_types();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_socket_options_by_type();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_rapid_create_close_types();
    total_passed += test_passed;
    total_failed += test_failed;
    
    // Summary
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║                      TEST SUMMARY                          ║\n");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║  Total Passed: %-12d                            ║\n", total_passed);
    printf("║  Total Failed: %-12d                            ║\n", total_failed);
    printf("╠════════════════════════════════════════════════════════════╣\n");
    if (total_failed == 0) {
        printf("║              ALL TESTS PASSED!                             ║\n");
    } else {
        printf("║              SOME TESTS FAILED                             ║\n");
    }
    printf("╚════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    return total_failed > 0 ? 1 : 0;
}
