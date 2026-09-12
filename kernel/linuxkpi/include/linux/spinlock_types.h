#ifndef __AVORY_LINUXKPI_SPINLOCK_TYPES_H
#define __AVORY_LINUXKPI_SPINLOCK_TYPES_H

/* Lock types for the native-backed LinuxKPI spinlock API.
 *
 * The structures are private to the layer: all access goes through
 * <linux/spinlock.h>, so the layout can differ from upstream's qspinlock.
 * Spinlocks carry the saved interrupt state so the plain spin_lock() variants
 * can mask interrupts (see spinlock.h for why that is stronger than Linux).
 */

#include <linux/types.h>

typedef struct spinlock {
  volatile unsigned int locked;
  unsigned long irq_flags;
} spinlock_t;

typedef struct raw_spinlock {
  volatile unsigned int locked;
  unsigned long irq_flags;
} raw_spinlock_t;

#define __SPIN_LOCK_UNLOCKED(lockname) {0, 0}
#define __RAW_SPIN_LOCK_UNLOCKED(lockname) {0, 0}

/* rwlock: positive = reader count, -1 = writer, 0 = free.  Only the writer
 * (exclusive) stores saved IRQ state. */
typedef struct rwlock {
  volatile int cnt;
  unsigned long irq_flags;
} rwlock_t;

#define __RW_LOCK_UNLOCKED(lockname) {0, 0}

typedef struct seqcount {
  volatile unsigned int sequence;
} seqcount_t;

typedef struct seqlock {
  spinlock_t lock;
  unsigned int sequence;
} seqlock_t;

#define __SEQLOCK_UNLOCKED(lockname) {{0, 0}, 0}

#endif /* __AVORY_LINUXKPI_SPINLOCK_TYPES_H */
