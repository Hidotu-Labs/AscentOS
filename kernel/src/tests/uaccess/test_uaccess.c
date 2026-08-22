#include "test_uaccess.h"
#include "../../console/klog.h"
#include "../../lib/string.h"
#include "../../lib/tsc.h"
#include <stdbool.h>
#include <stdint.h>

extern long strncpy_from_user(char *dst, const char *src, long count);
extern long strnlen_user(const char *src, long maxlen);
extern unsigned long copy_from_user(void *to, const void *from, unsigned long n);
extern unsigned long copy_to_user(void *to, const void *from, unsigned long n);

static int g_uaccess_fail = 0;

#define UACCESS_ASSERT(expr)                                                   \
  do {                                                                         \
    if (!(expr)) {                                                             \
      klog_puts(KLOG_CLR_RED "[UACCESS TEST] FAIL: " #expr KLOG_CLR_RESET       \
                             "\n");                                            \
      g_uaccess_fail = 1;                                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

void test_uaccess(void) {
  klog_puts(KLOG_CLR_BLUE "[TEST]" KLOG_CLR_RESET
                          " Running Fast Word-at-a-time uaccess Test Suite...\n");
  g_uaccess_fail = 0;

  // 1. Correctness across alignments (0..7) and string lengths
  char src_buf[512] __attribute__((aligned(16)));
  char dst_buf[512];

  for (int len = 0; len <= 128; len++) {
    for (int align = 0; align < 8; align++) {
      char *src = src_buf + align;
      memset(src_buf, 0xAA, sizeof(src_buf));
      memset(dst_buf, 0xBB, sizeof(dst_buf));

      for (int i = 0; i < len; i++) {
        src[i] = 'A' + (i % 26);
      }
      src[len] = '\0';

      long copied = strncpy_from_user(dst_buf, src, sizeof(dst_buf));
      UACCESS_ASSERT(copied == len);
      UACCESS_ASSERT(dst_buf[len] == '\0');
      if (len > 0) {
        UACCESS_ASSERT(memcmp(dst_buf, src, len) == 0);
      }

      long slen = strnlen_user(src, sizeof(src_buf));
      UACCESS_ASSERT(slen == (len + 1));
    }
  }
  klog_puts("  [UACCESS] strncpy_from_user & strnlen_user alignment/length verified (1032 cases)\n");

  // 2. Truncation when count < strlen
  const char *long_str = "0123456789abcdef0123456789abcdef";
  memset(dst_buf, 0, sizeof(dst_buf));
  long trunc_res = strncpy_from_user(dst_buf, long_str, 10);
  UACCESS_ASSERT(trunc_res == 10);
  UACCESS_ASSERT(memcmp(dst_buf, "0123456789", 10) == 0);
  klog_puts("  [UACCESS] String truncation bounds verified\n");

  // 3. Exception bounds check (-EFAULT on out of range)
  long fault_res = strncpy_from_user(dst_buf, (const char *)0xFFFF800000000000ULL, 64);
  UACCESS_ASSERT(fault_res == -14); // -EFAULT
  klog_puts("  [UACCESS] Canonical user space bounds protection verified\n");

  // 4. Performance Benchmark: 10,000 iterations copying a 128-byte path
  char bench_src[128];
  char bench_dst[128];
  memset(bench_src, 'x', sizeof(bench_src) - 1);
  bench_src[sizeof(bench_src) - 1] = '\0';

  const int iterations = 10000;
  uint64_t t0 = rdtsc_fence();
  for (int i = 0; i < iterations; i++) {
    strncpy_from_user(bench_dst, bench_src, sizeof(bench_dst));
  }
  uint64_t t1 = rdtsc_fence();
  uint64_t total_cycles = t1 - t0;
  uint64_t avg_cycles = total_cycles / iterations;

  klog_puts("  [UACCESS] 128-byte path copy benchmark (10,000 iterations):\n");
  klog_puts("            Average copy time: ");
  klog_uint64(avg_cycles);
  klog_puts(" cycles/path (");
  klog_uint64((avg_cycles * 1000) / 128);
  klog_puts(" millicycles/byte)\n");

  klog_puts(KLOG_CLR_GREEN "[PASS]" KLOG_CLR_RESET
                          " Fast Word-at-a-time uaccess tests PASSED successfully!\n\n");
}
