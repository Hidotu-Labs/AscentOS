// Lazy FPU / SSE Context Switching
//
// Instead of saving and restoring 512 bytes of FPU/SSE state on every context
// switch (fxsave64 + fxrstor64 = ~120 cycles), we use the hardware CR0.TS
// (Task Switched) bit:
//
//   1. On every context switch, set CR0.TS = 1.
//   2. When any thread executes an x87 or SSE instruction, the CPU raises
//      exception #NM (Interrupt 7, Device Not Available).
//   3. Our #NM handler then:
//        a. Saves old FPU state back into the previous owner's fpu_state buffer.
//        b. Restores the faulting thread's fpu_state into hardware registers.
//        c. Clears CR0.TS (clts) so the thread can continue.
//        d. Records this thread as fpu_owner on this CPU.
//   4. Threads that never touch FPU/SSE never pay the fxsave/fxrstor cost.
//
// This cuts context switch latency by ~40-50% for the common case of integer
// and memory-only threads (timers, futex waiters, I/O threads).

#include "fpu.h"
#include "../cpu/isr.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "../console/klog.h"
#include <stdint.h>

// Set CR0.TS bit — causes #NM on next FPU/SSE instruction
static inline void cr0_set_ts(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 3); // TS bit
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

// Clear CR0.TS — allow FPU/SSE instructions without fault (clts instruction)
static inline void cr0_clear_ts(void) {
    __asm__ volatile("clts" : : : "memory");
}

// Save the hardware FPU state into the thread's buffer.
// CR0.TS must NOT be set when this is called, or we'd fault.
static inline void fpu_save(struct thread *t) {
    __asm__ volatile("fxsave64 %0" : "=m"(t->fpu_state) : : "memory");
}

// Load the hardware FPU state from the thread's buffer.
// CR0.TS must NOT be set when this is called.
static inline void fpu_restore(struct thread *t) {
    __asm__ volatile("fxrstor64 %0" : : "m"(t->fpu_state) : "memory");
}

// Exception #NM (vector 7, Device Not Available) handler.
// Fires when CR0.TS=1 and a thread executes an x87/SSE/MMX instruction.
void fpu_nm_handler(struct registers *regs) {
    (void)regs;

    struct cpu_info *cpu = cpu_get_current();
    struct thread *next = cpu->current_thread;

    if (!next) {
        // Should not happen; kernel threads using FPU before scheduler is up.
        cr0_clear_ts();
        return;
    }

    struct thread *owner = cpu->fpu_owner;

    if (owner == next) {
        // We already own the FPU on this CPU — just re-enable it.
        // This can happen after a kernel path set TS without switching threads.
        cr0_clear_ts();
        return;
    }

    // Save the previous owner's live FPU state before evicting it.
    // We must clear TS first so the fxsave64 itself doesn't fault.
    cr0_clear_ts();

    if (owner) {
        fpu_save(owner);
    }

    // Load the new thread's FPU state into hardware registers.
    fpu_restore(next);

    // Record ownership.
    cpu->fpu_owner = next;

    // CR0.TS stays clear — this thread can now execute FPU/SSE freely
    // until the next context switch sets it again.
}

// Called from switch_context (via sched_switch) after the RSP swap.
// Sets CR0.TS so the arriving thread will fault on first FPU use.
// The fxsave/fxrstor is deferred until the thread actually uses FPU.
void fpu_on_context_switch(void) {
    cr0_set_ts();
}

// Called when a thread is about to be freed.
// If this CPU held the thread's FPU state, clear ownership so we don't
// restore garbage into hardware registers for the next thread.
void fpu_forget_thread(struct thread *t) {
    // Walk all CPUs since we don't know which CPU last ran this thread.
    for (uint32_t i = 0; i < cpu_get_count(); i++) {
        struct cpu_info *c = cpu_get_info(i);
        if (!c) continue;
        if (c->fpu_owner == t) {
            c->fpu_owner = NULL;
            // Don't bother clearing TS — it'll be set again on next switch anyway.
        }
    }
}

// Ensure the FPU state for thread `t` is loaded in hardware.
// Used by signal delivery which needs to fxsave the live FPU state.
void fpu_ensure_loaded(const struct thread *t) {
    struct cpu_info *cpu = cpu_get_current();
    if (cpu->fpu_owner == t) {
        return; // Already live, nothing to do.
    }
    // Not live — save the current owner, restore this thread's state.
    cr0_clear_ts();
    if (cpu->fpu_owner) {
        fpu_save(cpu->fpu_owner);
    }
    // Cast away const: fxrstor64 reads the buffer, does not write it.
    fpu_restore((struct thread *)(uintptr_t)t);
    cpu->fpu_owner = (struct thread *)(uintptr_t)t;
}


void fpu_init(void) {
    // Register #NM (vector 7) handler.
    register_interrupt_handler(7, fpu_nm_handler);
    klog_debug_puts("[FPU] Lazy FPU switching enabled (CR0.TS / #NM)\n");
}
