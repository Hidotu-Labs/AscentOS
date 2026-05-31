#include "lapic_timer.h"
#include "lapic.h"
#include "../cpu/isr.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../io/io.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"

// ── PIT constants for calibration ────────────────────────────────────────────
#define PIT_CMD_PORT   0x43
#define PIT_DATA_PORT0 0x40
#define PIT_BASE_FREQ  1193182  // PIT oscillator frequency in Hz
#define CALIBRATION_MS 10       // How long to measure (10 ms)

// ── State ────────────────────────────────────────────────────────────────────
static volatile uint64_t lapic_timer_ticks = 0;
static uint32_t          ticks_per_ms      = 0;   // LAPIC decrements per ms

// ── Helpers ──────────────────────────────────────────────────────────────────
static void print_hex32(uint32_t num) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        klog_putchar(hex[(num >> i) & 0xF]);
    }
}

// ── Timer ISR ────────────────────────────────────────────────────────────────
void lapic_timer_handler(struct registers *regs) {
    // Only the primary core increments the global system uptime.
    // This prevents time from running 4x faster on a 4-core system.
    struct cpu_info *cpu = cpu_get_current();
    if (cpu && cpu->status == CPU_STATUS_BSP) {
        lapic_timer_ticks++;
        /* Fire any expired timerfd instances */
        extern void timerfd_tick(void);
        timerfd_tick();
    }

    // Send EOI BEFORE context switch. This is a special case - normally
    // isr_handler sends EOI after the handler returns. But the scheduler
    // may switch to another thread, and we need to keep receiving timer
    // interrupts. Double EOI (here + in isr_handler) is harmless.
    lapic_write(LAPIC_EOI, 0);

    // Call the scheduler. Every core handles its own preemption.
    // Safety check: only yield if we have a valid cpu structure and a thread to switch from.
    if (cpu && cpu->current_thread) {
        sched_tick(regs);
    }
}

// ── PIT polling sleep for calibration ────────────────────────────────────────
// Uses the PIT counter-latch to busy-wait for a known duration.
// We program the PIT in mode 0 (one-shot) and poll the 16-bit counter until
// we detect it has wrapped (current > previous means the countdown finished).
static void pit_calibration_sleep_ms(uint32_t ms) {
    uint32_t divisor = (PIT_BASE_FREQ * ms) / 1000;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    // Channel 0, lobyte/hibyte access, mode 0, binary
    outb(PIT_CMD_PORT, 0x30);
    outb(PIT_DATA_PORT0, (uint8_t)(divisor & 0xFF));
    outb(PIT_DATA_PORT0, (uint8_t)((divisor >> 8) & 0xFF));

    // Poll: latch counter 0 and read 16-bit value.
    // In mode 0, counter counts from `divisor` down to 0 and then stops.
    // We detect completion when the counter has reached a very low value.
    uint16_t prev = (uint16_t)divisor;
    for (;;) {
        // Latch channel 0 counter (command byte: channel 0, latch count)
        outb(PIT_CMD_PORT, 0x00);
        uint8_t lo = inb(PIT_DATA_PORT0);
        uint8_t hi = inb(PIT_DATA_PORT0);
        uint16_t curr = (uint16_t)hi << 8 | lo;

        // If counter wrapped around (curr > prev) or reached 0, we're done
        if (curr > prev || curr == 0) break;
        prev = curr;
    }
}

// ── Calibration ──────────────────────────────────────────────────────────────
// We program the LAPIC timer to count down from 0xFFFFFFFF and measure how
// many decrements occur during a known PIT-based delay.
static uint32_t calibrate_lapic_timer(void) {
    // Set divider to 16 for a reasonable range
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);

    // Start the counter at max, one-shot (masked so it doesn't fire)
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    // Wait a known duration using the PIT
    pit_calibration_sleep_ms(CALIBRATION_MS);

    // Stop the LAPIC timer
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);

    // ticks_per_ms = elapsed / CALIBRATION_MS
    return elapsed / CALIBRATION_MS;
}

// ── Public API ───────────────────────────────────────────────────────────────

void lapic_timer_init(void) {
    klog_puts("[INFO] Calibrating LAPIC timer against PIT...\n");

    ticks_per_ms = calibrate_lapic_timer();

    klog_puts("     LAPIC ticks/ms: 0x");
    print_hex32(ticks_per_ms);
    klog_puts(" (");
    klog_uint64(ticks_per_ms);
    klog_puts(")\n");

    if (ticks_per_ms == 0) {
        klog_puts("[ERR] LAPIC timer calibration failed (0 ticks/ms). Aborting.\n");
        return;
    }

    // Register ISR for our timer vector
    register_interrupt_handler(LAPIC_TIMER_VECTOR, lapic_timer_handler);

    // Calculate initial count for 1 ms period (LAPIC_TIMER_HZ = 1000)
    uint32_t init_count = ticks_per_ms * (1000 / LAPIC_TIMER_HZ);

    // Set divider to 16 (same as calibration)
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);

    // Program periodic mode with our vector
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);

    // Start counting!
    lapic_write(LAPIC_TIMER_INIT, init_count);

    klog_puts("[OK] LAPIC Timer started: ");
    klog_uint64(LAPIC_TIMER_HZ);
    klog_puts(" Hz periodic (vector ");
    klog_uint64(LAPIC_TIMER_VECTOR);
    klog_puts(")\n");
}

void lapic_timer_init_ap(void) {
    if (ticks_per_ms == 0) return; // BSP should have calibrated this

    uint32_t init_count = ticks_per_ms * (1000 / LAPIC_TIMER_HZ);

    // Set divider to 16
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);

    // Program periodic mode with our vector
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);

    // Start counting
    lapic_write(LAPIC_TIMER_INIT, init_count);
}

uint64_t lapic_timer_get_ticks(void) {
    return lapic_timer_ticks;
}

uint64_t lapic_timer_get_ms(void) {
    // Each tick is 1 ms when LAPIC_TIMER_HZ == 1000
    return lapic_timer_ticks * (1000 / LAPIC_TIMER_HZ);
}

void lapic_timer_sleep(uint32_t ms) {
    uint64_t target = lapic_timer_ticks + ms;
    while (lapic_timer_ticks < target) {
        __asm__ volatile("hlt");
    }
}
