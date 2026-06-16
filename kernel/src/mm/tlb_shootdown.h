#ifndef MM_TLB_SHOOTDOWN_H
#define MM_TLB_SHOOTDOWN_H

#include <stdint.h>

// IPI vector reserved for TLB shootdown.
// Must not collide with IPI_VECTOR_RESCHEDULE (49) or LAPIC_SPURIOUS_VECTOR (0xFF).
#define IPI_VECTOR_TLB_SHOOTDOWN 50

// Sentinel: invalidate all pages (full CR3 reload) instead of a single invlpg.
#define TLB_SHOOTDOWN_ALL UINT64_MAX

// One-time init: register the IPI handler for IPI_VECTOR_TLB_SHOOTDOWN.
// Call this after isr_init_exceptions() and before cpu_init_aps().
void tlb_shootdown_init(void);

// Flush a single page on every other online CPU, then on the local CPU.
// If addr == TLB_SHOOTDOWN_ALL, a full CR3 reload is performed on all CPUs.
// Callers must hold the VMM spinlock (or equivalent) while the PTE is still
// in its intermediate state, so no CPU can observe a half-updated entry.
void tlb_shootdown_page(uint64_t addr);

// Flush all pages (full CR3 reload) on every CPU.
void tlb_shootdown_all(void);

// Called from the IPI handler — do not call directly.
void tlb_shootdown_handle_ipi(void);

#endif // MM_TLB_SHOOTDOWN_H
