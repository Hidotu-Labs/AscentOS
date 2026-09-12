/* LinuxKPI Phase 0 boot self-test.
 *
 * Proves the phase-0 plumbing end to end:
 *   - the pinned upstream Linux tree was imported and compiled into the
 *     kernel (lib/math/div64.c),
 *   - the kernel FPU begin/end section preserves XSAVE state,
 *   - an initcall registered through the Phase 0 mechanism runs at boot.
 */

#include <stdint.h>

#include "console/klog.h"
#include "linuxkpi/fpu.h"
#include "linuxkpi/initcall.h"

#ifdef LINUXKPI_LINUX_IMPORTED
/* On x86_64 most of div64.c is masked by inline definitions in
 * <linux/math64.h>; what this object actually exports here is
 * iter_div_u64_rem(), so that is the Phase 0 link smoke test.  The strong
 * reference keeps the upstream section alive through --gc-sections. */
extern uint32_t iter_div_u64_rem(uint64_t dividend, uint32_t divisor,
                                 uint64_t *remainder);
#endif

static int phase0_initcall_demo(void) {
  klog_puts("[LINUXKPI] initcall demo (Phase 0) running\n");
  return 0;
}
LINUXKPI_INITCALL(phase0_initcall_demo);

void linuxkpi_test_phase0(void) {
  klog_puts("[LINUXKPI] Phase 0 self-test\n");

#ifdef LINUXKPI_LINUX_IMPORTED
  uint64_t rem = 0;
  uint32_t q = iter_div_u64_rem(100, 7, &rem);
  if (q == 14 && rem == 2) {
    klog_puts("[  OK  ] LinuxKPI: upstream lib/math/div64.c linked, result "
              "correct\n");
  } else {
    klogf("[ FAIL ] LinuxKPI: iter_div_u64_rem(100, 7) = %u rem %llu\n",
          q, (unsigned long long)rem);
  }
#else
  klog_puts("[ WARN ] LinuxKPI: Linux tree not imported; div64 test skipped "
            "(run scripts/linux-import.sh)\n");
#endif

  if (linuxkpi_fpu_selftest()) {
    klog_puts("[  OK  ] LinuxKPI: kernel_fpu_begin/end restores XSAVE state\n");
  } else {
    klog_puts("[ FAIL ] LinuxKPI: FPU state was not restored\n");
  }
}
