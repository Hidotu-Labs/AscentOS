#include "tlb_shootdown.h"
#include "../apic/lapic.h"
#include "../console/klog.h"
#include "../cpu/isr.h"
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

    // Count online remote CPUs.
    uint32_t targets = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!c) continue;
        if (c == cpu_get_current()) continue;
        if (c->status == CPU_STATUS_ONLINE || c->status == CPU_STATUS_BSP)
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
        if (!c) continue;
        if (c == cpu_get_current()) continue;
        if (c->status != CPU_STATUS_ONLINE && c->status != CPU_STATUS_BSP)
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
