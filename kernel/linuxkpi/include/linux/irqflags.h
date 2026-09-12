#ifndef __AVORY_LINUXKPI_IRQFLAGS_H
#define __AVORY_LINUXKPI_IRQFLAGS_H

/* Native-backed Linux <linux/irqflags.h> overlay.
 *
 * The upstream header pulls in asm/nospec-branch.h -> asm/percpu.h ->
 * kernel.h/per-CPU machinery that does not exist here yet.  IRQ masking is a
 * native HAL primitive, so the overlay is small and exact.  IRQ flags are the
 * raw RFLAGS value; a zero flag means interrupts were disabled on entry.
 */

#include <linuxkpi/native_irq.h>

#define local_irq_save(flags)                                                 \
  do {                                                                        \
    (flags) = asc_irq_save();                                                 \
  } while (0)
#define local_irq_restore(flags) asc_irq_restore(flags)
#define local_irq_disable() asc_irq_disable()
#define local_irq_enable() asc_irq_enable()

#define raw_local_irq_save(flags) local_irq_save(flags)
#define raw_local_irq_restore(flags) local_irq_restore(flags)
#define raw_local_irq_disable() local_irq_disable()
#define raw_local_irq_enable() local_irq_enable()

#define local_save_flags(flags)                                               \
  do {                                                                        \
    (flags) = linuxkpi_irq_flags();                                           \
  } while (0)

#define local_irq_is_disabled() (!asc_irq_enabled())
#define irqs_disabled() (!asc_irq_enabled())
#define irqs_disabled_flags(flags) (((flags) & 0x200UL) == 0)

#define arch_local_irq_save(flags) local_irq_save(flags)
#define arch_local_irq_restore(flags) local_irq_restore(flags)

#endif /* __AVORY_LINUXKPI_IRQFLAGS_H */
