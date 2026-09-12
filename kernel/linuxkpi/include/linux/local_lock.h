#ifndef __AVORY_LINUXKPI_LOCAL_LOCK_H
#define __AVORY_LINUXKPI_LOCAL_LOCK_H

/* No-op local (per-CPU) locks.  Linux uses these to protect per-CPU data
 * without disabling preemption; AvoryOS has no per-CPU preemption, so the
 * lock/unlock pairs are inert and only keep call sites balanced. */

#include <linux/types.h>

typedef struct {
  int unused;
} local_lock_t;

#define INIT_LOCAL_LOCK(lock) {0}

static inline void local_lock_init(local_lock_t *l) { (void)l; }
static inline void local_lock(local_lock_t *l) { (void)l; }
static inline void local_unlock(local_lock_t *l) { (void)l; }
static inline void local_lock_irq(local_lock_t *l) { (void)l; }
static inline void local_unlock_irq(local_lock_t *l) { (void)l; }
static inline void local_lock_irqsave(local_lock_t *l, unsigned long *flags) {
  (void)l;
  (void)flags;
}
static inline void local_unlock_irqrestore(local_lock_t *l,
                                           unsigned long *flags) {
  (void)l;
  (void)flags;
}

#endif /* __AVORY_LINUXKPI_LOCAL_LOCK_H */
