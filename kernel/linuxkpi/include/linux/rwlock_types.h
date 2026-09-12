#ifndef __AVORY_LINUXKPI_RWONCE_TYPE_SHIMS_H
#define __AVORY_LINUXKPI_RWONCE_TYPE_SHIMS_H

/* Headers that include the type-only rwlock/seqlock headers directly must see
 * the same types as <linux/spinlock.h>; route them to the overlay. */
#include <linux/spinlock_types.h>

#endif
