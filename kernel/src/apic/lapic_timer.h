#ifndef APIC_LAPIC_TIMER_H
#define APIC_LAPIC_TIMER_H

#include <stdint.h>

// LAPIC Timer Configuration
// The timer fires at this vector.  Must not collide with existing IRQ vectors
// (32-47) or the spurious vector (255).
#define LAPIC_TIMER_VECTOR 48

// Target frequency in Hz — how many times per second the timer fires.
#define LAPIC_TIMER_HZ 1000

// Public API

// Calibrate the LAPIC timer against the PIT, then start it in periodic mode.
// Must be called after lapic_init() and with interrupts enabled.
void lapic_timer_init(void);

// Initialize the LAPIC timer on an AP.
void lapic_timer_init_ap(void);

// Returns total ticks since the LAPIC timer was started.
uint64_t lapic_timer_get_ticks(void);

// Returns uptime in milliseconds.
uint64_t lapic_timer_get_ms(void);

// Sleep for approximately `ms` milliseconds using the LAPIC timer.
void lapic_timer_sleep(uint32_t ms);

#endif
