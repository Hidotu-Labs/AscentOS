#ifndef LINUXKPI_NATIVE_IRQ_H
#define LINUXKPI_NATIVE_IRQ_H

/* Native IRQ-state bridge (kernel/src/hal/hal.h via asm aliases). */

extern unsigned long asc_irq_save(void) __asm__("hal_irq_save");
extern void asc_irq_restore(unsigned long flags) __asm__("hal_irq_restore");
extern void asc_irq_disable(void) __asm__("hal_irq_disable");
extern void asc_irq_enable(void) __asm__("hal_irq_enable");
extern _Bool asc_irq_enabled(void) __asm__("hal_irq_enabled");

/* Current RFLAGS (without modifying the IF state). */
unsigned long linuxkpi_irq_flags(void);

#endif /* LINUXKPI_NATIVE_IRQ_H */
