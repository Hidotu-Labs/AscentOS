#ifndef __AVORY_LINUXKPI_LOCKDEP_H
#define __AVORY_LINUXKPI_LOCKDEP_H

/* No-op lock dependency tracking.  Many upstream headers include
 * <linux/lockdep.h> and expect the assertion macros to exist; without
 * CONFIG_LOCKDEP they are all no-ops, which is what this overlay provides
 * without dragging in sched.h/thread_info.h. */

#include <linux/types.h>

static inline void lockdep_init(void) {}
static inline void lockdep_off(void) {}
static inline void lockdep_on(void) {}

#define lockdep_assert_held(l) ((void)(l))
#define lockdep_assert_held_once(l) ((void)(l))
#define lockdep_assert_not_held(l) ((void)(l))
#define lockdep_assert_irqs_enabled()                                          \
  do {                                                                        \
  } while (0)
#define lockdep_assert_irqs_disabled()                                         \
  do {                                                                        \
  } while (0)
#define lockdep_assert_preemption_enabled()                                    \
  do {                                                                        \
  } while (0)
#define lockdep_assert_preemption_disabled()                                   \
  do {                                                                        \
  } while (0)

#define lockdep_is_held(l) (1)
#define lockdep_is_held_type(l, t)                                             \
  ((void)(t), 1)
#define lockdep_assert_held_write(l) ((void)(l))
#define lockdep_assert_held_read(l) ((void)(l))

#define lockdep_set_class(l, c)                                                \
  do {                                                                        \
    (void)(l);                                                                 \
    (void)(c);                                                                 \
  } while (0)
#define lockdep_set_subclass(l, s)                                             \
  do {                                                                        \
    (void)(l);                                                                 \
    (void)(s);                                                                 \
  } while (0)

#endif /* __AVORY_LINUXKPI_LOCKDEP_H */
