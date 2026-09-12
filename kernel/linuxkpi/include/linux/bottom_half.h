#ifndef __AVORY_LINUXKPI_BOTTOM_HALF_H
#define __AVORY_LINUXKPI_BOTTOM_HALF_H

/* Linux <linux/bottom_half.h> overlay.
 *
 * AvoryOS has no softirq mechanism; imported code that pairs
 * local_bh_disable()/local_bh_enable() simply keeps the preemption counter
 * balanced, so it neither sleeps nor gets preempted in ways Linux code
 * assumes.  A real softirq layer, if ever needed, replaces this. */

#include <linux/preempt.h>

static inline void local_bh_disable(void) { preempt_count_add(1); }
static inline void local_bh_enable(void) { preempt_count_sub(1); }

#define in_softirq() (false)
#define in_serving_softirq() (false)

#endif /* __AVORY_LINUXKPI_BOTTOM_HALF_H */
