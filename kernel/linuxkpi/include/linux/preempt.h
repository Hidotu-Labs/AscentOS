#ifndef __AVORY_LINUXKPI_PREEMPT_H
#define __AVORY_LINUXKPI_PREEMPT_H

/* Native-backed Linux <linux/preempt.h> overlay.
 *
 * Linux tracks preemption (and IRQ/softirq nesting) in per-task/pre-CPU
 * counters.  AvoryOS gets the preemption bit now, backed by a per-CPU counter
 * in linuxkpi/src/preempt.c; hardirq/softirq context tracking arrives with
 * the interrupt-layer work (in_interrupt() is still false).  The counter is
 * what spinlocks rely on, so the semantics that matter today are honored:
 * preempt_disable() nesting and preempt_count() != 0 meaning atomic context.
 */

#include <linux/compiler.h>
#include <linux/types.h>

unsigned int __kpi_preempt_count(void);
void __kpi_preempt_add(int val);
void __kpi_preempt_sub(int val);
void __kpi_preempt_disable(void);
void __kpi_preempt_enable(void);

#define preempt_count() (__kpi_preempt_count())
#define preempt_disable() __kpi_preempt_disable()
#define preempt_enable() __kpi_preempt_enable()
#define preempt_disable_notrace() preempt_disable()
#define preempt_enable_notrace() preempt_enable()
#define preempt_enable_no_resched() preempt_enable()

static inline void preempt_count_add(int val) { __kpi_preempt_add(val); }
static inline void preempt_count_sub(int val) { __kpi_preempt_sub(val); }

static inline bool in_atomic(void) { return preempt_count() != 0; }
static inline bool in_softirq(void) { return false; }
static inline bool in_interrupt(void) { return false; }
static inline bool in_serving_softirq(void) { return false; }

/* Context checks are advisory until the scheduler/IRQ integration lands.
 * might_sleep()/might_sleep_if() live in <linux/kernel.h> upstream (they call
 * might_resched()); do not redefine them here. */
#define might_resched() do { } while (0)
static inline void cond_resched(void) { }

static inline void preempt_fold_need_resched(void) { }

#endif /* __AVORY_LINUXKPI_PREEMPT_H */
