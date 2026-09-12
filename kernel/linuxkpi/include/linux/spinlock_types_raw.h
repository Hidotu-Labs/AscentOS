#ifndef __AVORY_LINUXKPI_SPINLOCK_TYPES_RAW_H
#define __AVORY_LINUXKPI_SPINLOCK_TYPES_RAW_H

/* Upstream defines struct raw_spinlock here and has headers such as
 * ratelimit_types.h include it directly.  Route everyone to the overlay so a
 * single raw_spinlock_t definition exists per translation unit. */
#include <linux/spinlock_types.h>

#endif
