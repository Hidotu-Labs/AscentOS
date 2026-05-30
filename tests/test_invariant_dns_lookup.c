#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * We simulate the vulnerable behavior in dns_lookup.c:
 *   - A fixed-size host buffer of MAXHOSTNAMELEN (typically 256 bytes)
 *   - The code does: strcat((char*)host, ".") without checking available space
 *
 * Security invariant: After processing any hostname input, the resulting
 * string (including the appended '.') must never exceed the declared buffer
 * length. The input must be truncated or rejected before the append if it
 * would cause an overflow.
 */

#define HOST_BUFFER_SIZE 256

/*
 * Safe version of the hostname processing that SHOULD be implemented.
 * Returns 0 on success (input accepted and '.' appended within bounds),
 * returns -1 if input would overflow (input rejected).
 */
static int safe_process_hostname(const char *input, char *out_buf, size_t buf_size) {
    if (input == NULL || out_buf == NULL || buf_size == 0) {
        return -1;
    }

    size_t input_len = strlen(input);

    /*
     * We need room for the input + '.' + '\0'
     * So input_len must be <= buf_size - 2
     */
    if (input_len > buf_size - 2) {
        /* Input too long: reject it */
        return -1;
    }

    /* Safe to copy and append */
    strncpy(out_buf, input, buf_size - 1);
    out_buf[buf_size - 1] = '\0';
    strncat(out_buf, ".", buf_size - strlen(out_buf) - 1);

    return 0;
}

START_TEST(test_buffer_read_never_exceeds_declared_length)
{
    /* Invariant: Buffer reads/writes never exceed the declared HOST_BUFFER_SIZE.
     * Any hostname input that would cause the buffer (including the appended '.')
     * to exceed HOST_BUFFER_SIZE must be rejected or truncated safely.
     */
    const char *payloads[] = {
        /* Exactly at limit (255 chars) - should be rejected (no room for '.') */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", /* 255 */

        /* Exactly at limit - 1 (254 chars) - should be rejected (no room for '.') */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", /* 254 */

        /* 2x buffer size (512 chars) */
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",

        /* 10x buffer size (2560 chars) */
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",

        /* Null byte injection attempt */
        "evil\x00AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",

        /* Format string attack payload */
        "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",

        /* Path traversal + overflow */
        "../../../../../../../../../../../../../../../../../../../../../../../../"
        "../../../../../../../../../../../../../../../../../../../../../../../../"
        "../../../../../../../../../../../../../../../../../../../../../../../../"
        "../../../../../../../../../../../../../../../../../../../../../../../../",

        /* Valid short hostname - should succeed */
        "example.com",

        /* Valid hostname exactly at safe limit (253 chars - valid DNS max) */
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",

        /* Empty string - edge case */
        "",

        /* Single character */
        "a",

        /* Hostname with special characters */
        "host\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    };

    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        char host_buffer[HOST_BUFFER_SIZE];
        memset(host_buffer, 0, sizeof(host_buffer));

        /* Canary bytes placed immediately after the buffer to detect overflow */
        uint8_t canary[8];
        memset(canary, 0xAB, sizeof(canary));

        size_t input_len = strlen(payloads[i]);
        int result = safe_process_hostname(payloads[i], host_buffer, HOST_BUFFER_SIZE);

        if (result == 0) {
            /* Input was accepted: verify the result fits within the buffer */
            size_t result_len = strlen(host_buffer);

            /* The resulting string must fit within the buffer (including null terminator) */
            ck_assert_msg(result_len < HOST_BUFFER_SIZE,
                "OVERFLOW: result string length %zu >= buffer size %zu for payload index %d",
                result_len, (size_t)HOST_BUFFER_SIZE, i);

            /* The result must end with '.' */
            ck_assert_msg(result_len > 0 && host_buffer[result_len - 1] == '.',
                "INVARIANT VIOLATION: accepted hostname does not end with '.' for payload index %d",
                i);

            /* The original input (without '.') must fit in the buffer */
            ck_assert_msg(input_len <= HOST_BUFFER_SIZE - 2,
                "INVARIANT VIOLATION: accepted input of length %zu that would overflow buffer of size %zu at index %d",
                input_len, (size_t)HOST_BUFFER_SIZE, i);
        } else {
            /* Input was rejected: verify it was indeed too long */
            ck_assert_msg(input_len > HOST_BUFFER_SIZE - 2,
                "INVARIANT VIOLATION: valid input of length %zu was incorrectly rejected for payload index %d",
                input_len, i);

            /* Buffer should remain clean (zeroed) after rejection */
            ck_assert_msg(host_buffer[0] == '\0',
                "INVARIANT VIOLATION: buffer was modified after rejection for payload index %d", i);
        }

        /* Verify canary bytes are intact (no overflow into adjacent memory) */
        for (int j = 0; j < (int)sizeof(canary); j++) {
            ck_assert_msg(canary[j] == 0xAB,
                "BUFFER OVERFLOW DETECTED: canary byte %d corrupted (0x%02x) for payload index %d",
                j, canary[j], i);
        }
    }
}
END_TEST

/*
 * Additional test: verify that a hostname at exactly HOST_BUFFER_SIZE - 2
 * (the maximum safe length) is accepted, and one at HOST_BUFFER_SIZE - 1
 * is rejected.
 */
START_TEST(test_boundary_conditions)
{
    /* Invariant: Boundary inputs at exactly buffer_size-2 are accepted,
     * inputs at buffer_size-1 and above are rejected.
     */
    char max_safe_input[HOST_BUFFER_SIZE];
    char too_long_