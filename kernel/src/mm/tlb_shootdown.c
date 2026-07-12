#include "tlb_shootdown.h"
#include "../apic/lapic.h"
#include "../console/klog.h"
#include "../cpu/isr.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "vmm.h"
#include "../lock/spinlock.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Serialise concurrent shootdown callers.
static spinlock_t shootdown_lock = SPINLOCK_INIT;

// Virtual address to invalidate.  TLB_SHOOTDOWN_ALL means full CR3 reload.
static volatile uint64_t shootdown_addr = 0;

// Number of target CPUs that still need to acknowledge.
static volatile uint32_t ack_pending = 0;

// User translations are private to a page table, so only CPUs currently
// running the same CR3 can hold a stale entry. Kernel mappings are shared in
// every address space and must still be invalidated on every online CPU.
static bool cpu_needs_shootdown(struct cpu_info *cpu, struct cpu_info *self,
                                uint64_t addr, uint64_t source_cr3) {
    if (!cpu || cpu == self)
        return false;
    if (cpu->status != CPU_STATUS_ONLINE && cpu->status != CPU_STATUS_BSP)
        return false;
    if (addr == TLB_SHOOTDOWN_ALL || (addr & (1ULL << 63)))
        return true;

    struct thread *thread =
        __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE);
    uint64_t target_cr3 = thread && thread->cr3 ? thread->cr3 : cpu->kernel_cr3;
    return (target_cr3 & ~0xFFFULL) == (source_cr3 & ~0xFFFULL);
}

void tlb_shootdown_handle_ipi(void) {
    uint64_t addr = shootdown_addr; // read before ack

    if (addr == TLB_SHOOTDOWN_ALL) {
        // Full CR3 reload flushes all TLB entries including globals.
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    } else {
        __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
    }

    // Signal the initiator that this CPU is done.
    __atomic_fetch_sub(&ack_pending, 1, __ATOMIC_RELEASE);
}

static void tlb_shootdown_isr(struct registers *regs) {
    (void)regs;
    tlb_shootdown_handle_ipi();
}


void tlb_shootdown_init(void) {
    register_interrupt_handler(IPI_VECTOR_TLB_SHOOTDOWN, tlb_shootdown_isr);
    klog_puts("[TLB] Shootdown IPI handler registered on vector 0x");
    klog_hex32(IPI_VECTOR_TLB_SHOOTDOWN);
    klog_puts("\n");
}

// Core implementation shared by tlb_shootdown_page() and tlb_shootdown_all().
static void do_shootdown(uint64_t addr) {
    uint32_t cpu_count = cpu_get_count();
    struct cpu_info *self = cpu_get_current();
    uint64_t source_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(source_cr3));

    // Count online remote CPUs.
    uint32_t targets = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (cpu_needs_shootdown(c, self, addr, source_cr3))
            targets++;
    }

    if (targets == 0) {
        // Uniprocessor — just flush locally.
        if (addr == TLB_SHOOTDOWN_ALL) {
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
        } else {
            __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
        }
        return;
    }

    spinlock_acquire(&shootdown_lock);

    // Publish the address before sending any IPI so target CPUs always see
    // the correct value when their handler fires.
    shootdown_addr = addr;
    __atomic_store_n(&ack_pending, targets, __ATOMIC_RELEASE);
    __asm__ volatile("" ::: "memory"); // compiler barrier

    // Send IPI to every online remote CPU.
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!cpu_needs_shootdown(c, self, addr, source_cr3))
            continue;
        lapic_send_ipi(c->apic_id, IPI_VECTOR_TLB_SHOOTDOWN);
    }

    // Wait for all targets to acknowledge.
    while (__atomic_load_n(&ack_pending, __ATOMIC_ACQUIRE) != 0)
        __asm__ volatile("pause" ::: "memory");

    // Now flush locally.
    if (addr == TLB_SHOOTDOWN_ALL) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    } else {
        __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
    }

    spinlock_release(&shootdown_lock);
}

void tlb_shootdown_page(uint64_t addr) {
    do_shootdown(addr);
}

void tlb_shootdown_all(void) {
    do_shootdown(TLB_SHOOTDOWN_ALL);
}
