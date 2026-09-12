#ifndef __AVORY_LINUXKPI_SCHED_MM_H
#define __AVORY_LINUXKPI_SCHED_MM_H

/* Minimal Linux <linux/sched/mm.h> overlay.
 *
 * Imported headers (xarray.h among them) include this for GFP-context and
 * mm-lifetime helpers; pulling in the real header drags full <linux/sched.h>
 * and the per-CPU/thread_info machinery AvoryOS does not have yet.  Only the
 * helpers are provided; mm_struct-based functions arrive with the memory
 * phase. */

#include <linux/types.h>
#include <linux/gfp.h>

static inline gfp_t current_gfp_context(gfp_t flags) { return flags; }

static inline void fs_reclaim_acquire(gfp_t gfp_mask) { (void)gfp_mask; }
static inline void fs_reclaim_release(gfp_t gfp_mask) { (void)gfp_mask; }

static inline void memalloc_retry_wait(gfp_t gfp_flags) { (void)gfp_flags; }
static inline void might_alloc(gfp_t gfp_mask) { (void)gfp_mask; }

/* Scope save/restore helpers: no scopes exist yet, so these are inert but
 * keep call sites balanced. */
static inline unsigned int memalloc_noio_save(void) { return 0; }
static inline void memalloc_noio_restore(unsigned int flags) { (void)flags; }
static inline unsigned int memalloc_noreclaim_save(void) { return 0; }
static inline void memalloc_noreclaim_restore(unsigned int flags) {
  (void)flags;
}
static inline unsigned int memalloc_nofs_save(void) { return 0; }
static inline void memalloc_nofs_restore(unsigned int flags) { (void)flags; }
static inline unsigned int memalloc_nowait_save(void) { return 0; }
static inline void memalloc_nowait_restore(unsigned int flags) { (void)flags; }

#endif /* __AVORY_LINUXKPI_SCHED_MM_H */
