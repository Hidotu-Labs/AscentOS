/* Initcall walker, Phase 0.  See linuxkpi/initcall.h for the mechanism. */

#include <stddef.h>
#include <stdint.h>

#include "console/klog.h"
#include "linuxkpi/initcall.h"

extern linuxkpi_initcall_fn_t __initcall_start[];
extern linuxkpi_initcall_fn_t __initcall_end[];

void linuxkpi_run_initcalls(void) {
  size_t total = (size_t)(__initcall_end - __initcall_start);

  if (total == 0) {
    klog_puts("[KERNEL] LinuxKPI: no initcalls registered\n");
    return;
  }

  klogf("[KERNEL] LinuxKPI: running %llu initcall(s)...\n",
        (unsigned long long)total);

  size_t failed = 0;
  for (size_t i = 0; i < total; i++) {
    linuxkpi_initcall_fn_t fn = __initcall_start[i];
    if (!fn)
      continue;
    int rc = fn();
    if (rc != 0) {
      failed++;
      klogf("[KERNEL] LinuxKPI: initcall #%llu returned %d\n",
            (unsigned long long)i, rc);
    }
  }

  if (failed) {
    klogf("[KERNEL] LinuxKPI: %llu initcall(s) failed\n",
          (unsigned long long)failed);
  } else {
    klog_puts("[KERNEL] LinuxKPI: all initcalls completed\n");
  }
}
