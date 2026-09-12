#ifndef LINUXKPI_FPU_H
#define LINUXKPI_FPU_H

#include <stdbool.h>

/* Kernel FPU access, Phase 0.
 *
 * Upstream Linux permits short SIMD sections in kernel code via
 * kernel_fpu_begin()/kernel_fpu_end() (declared in <asm/fpu/api.h>); the AMD
 * display DML code calls them through DC_FP_START/DC_FP_END.  AvoryOS compiles
 * the kernel with -mno-sse, and imported float-heavy files are compiled with
 * SSE enabled individually, so these entry points are what makes that safe.
 *
 * The implementation in linuxkpi/fpu.c saves the full XSAVE state of the
 * calling CPU to a per-CPU buffer, masks interrupts for the duration so the
 * thread cannot be preempted out from under the buffer, and restores on the
 * matching end call.  Calls may nest; only the outermost pair saves/restores.
 *
 * Context: only valid after per-CPU data is initialized (BSP/AP bring-up);
 * kernel_fpu_begin() must not be called from interrupt context.
 */

void kernel_fpu_begin(void);
void kernel_fpu_end(void);

/* x86 Linux reports whether FPU use is currently legal (not in IRQ/NMI with
 * an FPU-owning context).  AvoryOS has no lazy FPU, so this is always true. */
bool irq_fpu_usable(void);

/* Phase 0 boot self-test: loads an XMM register with a pattern, runs a short
 * begin/end section that clobbers it, and verifies the pattern survived.
 * Returns true on success. */
bool linuxkpi_fpu_selftest(void);

#endif /* LINUXKPI_FPU_H */
