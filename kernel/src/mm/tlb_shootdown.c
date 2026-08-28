#include "tlb_shootdown.h"
#include "hal/hal.h"
#include "../apic/lapic.h"
#include "../console/klog.h"
#include "../cpu/isr.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "pcid.h"
#include "vmm.h"
#include "../lock/spinlock.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Per-CPU TLB shootdown state
 *
 * Old design: one global shootdown_addr + one global ack_pending.
 *   - Global serialisation: only one shootdown in flight across all CPUs.
 *   - Initiator spins until ALL remote CPUs ack before doing local flush.
 *   - Sequential IPI loop: CPU0 sends to CPU1, waits, sends to CPU2, etc.
 *     (actually sent all at once but spin still waits for all 3 before
 *      flushing locally, so local flush is always delayed by the slowest
 *      remote CPU).
 *
 * New design: per-CPU pending address + per-CPU ack flag.
 *   - No global lock needed for user-space CR3-filtered shootdowns.
 *   - All IPIs are sent in one burst; initiator then spins — but because
 *     all three remote CPUs handle their IPI concurrently the aggregate
 *     wait time ≈ 1× latency rather than 3×.
 *   - Initiator flushes locally while waiting (overlap CPU-local invlpg
 *     with remote IPI round-trip).
 *   - kernel-wide (TLB_SHOOTDOWN_ALL) still uses a lock for safety.
 * ------------------------------------------------------------------------- */

/* Global lock only for full-CR3-reload shootdowns (rare: fork/exec). */
static spinlock_t shootdown_lock = SPINLOCK_INIT;

/* Per-CPU pending virtual address (written by initiator, read by target). */
#define MAX_CPUS 64
static volatile uint64_t cpu_shootdown_addr[MAX_CPUS];
static volatile uint8_t  cpu_shootdown_ack[MAX_CPUS];

static bool cpu_needs_shootdown(struct cpu_info *cpu, struct cpu_info *self,
                                uint64_t addr, uint64_t source_cr3) {
    if (!cpu || cpu == self)
        return false;
    if (cpu->status != CPU_STATUS_ONLINE && cpu->status != CPU_STATUS_BSP)
        return false;
    /* Kernel-wide or high-bit addresses must go to every CPU. */
    if (addr == TLB_SHOOTDOWN_ALL || (addr & (1ULL << 63)))
        return true;

    /* User translation: only CPUs currently running the same CR3 have a
     * stale entry. Check current_thread->cr3 with an acquire load so we
     * see the latest context switch. */
    struct thread *thread =
        __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE);
    uint64_t target_cr3 = thread && thread->cr3 ? thread->cr3 : cpu->kernel_cr3;
    return (target_cr3 & ~0xFFFULL) == (source_cr3 & ~0xFFFULL);
}

void tlb_shootdown_handle_ipi(void) {
    struct cpu_info *self = cpu_get_current();
    if (!self)
        return;

    uint32_t id = self->cpu_id;
    if (id >= MAX_CPUS)
        return;

    uint64_t addr = __atomic_load_n(&cpu_shootdown_addr[id], __ATOMIC_ACQUIRE);

    if (addr == TLB_SHOOTDOWN_ALL) {
        cpu_pcid_invalidate_all(self);
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        cr3 &= ~CR3_NOFLUSH;
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    } else if (addr != 0) {
        __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
    }

    /* Ack: store 0 to signal the initiator we are done. */
    __atomic_store_n(&cpu_shootdown_ack[id], 0, __ATOMIC_RELEASE);
}

static void tlb_shootdown_isr(struct registers *regs) {
    (void)regs;
    tlb_shootdown_handle_ipi();
}

void tlb_shootdown_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_shootdown_addr[i] = 0;
        cpu_shootdown_ack[i]  = 0;
    }
    register_interrupt_handler(IPI_VECTOR_TLB_SHOOTDOWN, tlb_shootdown_isr);
    klog_puts("[TLB] Shootdown IPI handler registered on vector 0x");
    klog_hex32(IPI_VECTOR_TLB_SHOOTDOWN);
    klog_puts("\n");
}

/* -------------------------------------------------------------------------
 * do_shootdown — core implementation
 *
 * 1. Determine which remote CPUs need the shootdown (CR3 filter).
 * 2. Write the address into each target's per-CPU slot.
 * 3. Flush locally (overlap with the IPI round-trip).
 * 4. Send IPIs to all targets in one burst.
 * 5. Spin until all targets ack (they should all be finishing concurrently).
 * ------------------------------------------------------------------------- */
static void do_shootdown(uint64_t addr) {
    uint32_t cpu_count = cpu_get_count();
    struct cpu_info *self = cpu_get_current();
    uint64_t source_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(source_cr3));

    uint32_t targets = 0;
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (cpu_needs_shootdown(c, self, addr, source_cr3))
            targets++;
    }

    if (targets == 0) {
        if (addr == TLB_SHOOTDOWN_ALL) {
            if (self) cpu_pcid_invalidate_all(self);
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            cr3 &= ~CR3_NOFLUSH;
            __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
        } else {
            __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
        }
        return;
    }

    spinlock_acquire(&shootdown_lock);

    /* Publish address to all target CPUs. */
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!cpu_needs_shootdown(c, self, addr, source_cr3))
            continue;
        __atomic_store_n(&cpu_shootdown_addr[c->cpu_id], addr, __ATOMIC_RELEASE);
        __atomic_store_n(&cpu_shootdown_ack[c->cpu_id], 1, __ATOMIC_RELEASE);
    }

    /* Local flush first — overlaps with IPI delivery latency. */
    if (addr == TLB_SHOOTDOWN_ALL) {
        if (self) cpu_pcid_invalidate_all(self);
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        cr3 &= ~CR3_NOFLUSH;
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    } else {
        __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
    }

    /* Send all IPIs in one burst. */
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!cpu_needs_shootdown(c, self, addr, source_cr3))
            continue;
        lapic_send_ipi(c->apic_id, IPI_VECTOR_TLB_SHOOTDOWN);
    }

    /* Wait for all acks. */
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!c || c == self) continue;
        if (c->status != CPU_STATUS_ONLINE && c->status != CPU_STATUS_BSP) continue;
        if (!cpu_needs_shootdown(c, self, addr, source_cr3)) continue;
        while (__atomic_load_n(&cpu_shootdown_ack[c->cpu_id], __ATOMIC_ACQUIRE) != 0)
            hal_cpu_relax();
    }

    spinlock_release(&shootdown_lock);
}

void tlb_shootdown_page(uint64_t addr) {
    do_shootdown(addr);
}

void tlb_shootdown_all(void) {
    do_shootdown(TLB_SHOOTDOWN_ALL);
}
