#ifndef LOCK_SPINLOCK_H
#define LOCK_SPINLOCK_H

#include "hal/hal.h"
#include <stdbool.h>
#include <stdint.h>

// Ticket Spinlock with TTAS & PAUSE backoff
// Provides:
// 1. Strict FIFO fairness (no CPU starvation on multi-core SMP).
// 2. Read-only spinning in local L1/L2 cache (eliminates cacheline bouncing and bus locks).
// 3. Hardware PAUSE backoff (reduces pipeline power and stall latency).
typedef struct {
  union {
    uint32_t val;
    struct {
      uint16_t now_serving;
      uint16_t next_ticket;
    };
  };
  hal_irq_state_t saved_flags;
} spinlock_t;

#define SPINLOCK_INIT { .val = 0, .saved_flags = 0 }

static inline void spinlock_init(spinlock_t *lock) {
  lock->val = 0;
  lock->saved_flags = 0;
}

// Interrupt-safe ticket spinlock acquire:
// 1. Save current RFLAGS (preserving IF state)
// 2. Disable interrupts (cli) so no timer tick can preempt us
// 3. Atomically claim our ticket number
// 4. Spin read-only with PAUSE until now_serving == our ticket
static inline void spinlock_acquire(spinlock_t *lock) {
  hal_irq_state_t flags = hal_irq_save();

  uint16_t my_ticket = __atomic_fetch_add(&lock->next_ticket, 1, __ATOMIC_RELAXED);

  while (__atomic_load_n(&lock->now_serving, __ATOMIC_ACQUIRE) != my_ticket) {
    __asm__ volatile("pause" ::: "memory");
  }

  lock->saved_flags = flags;
}

static inline void spinlock_release(spinlock_t *lock) {
  hal_irq_state_t flags = lock->saved_flags;
  uint16_t serving = lock->now_serving + 1;
  __atomic_store_n(&lock->now_serving, serving, __ATOMIC_RELEASE);
  hal_irq_restore(flags);
}

static inline void spinlock_acquire_save(spinlock_t *lock, uint64_t *flags) {
  *flags = hal_irq_save();
  uint16_t my_ticket = __atomic_fetch_add(&lock->next_ticket, 1, __ATOMIC_RELAXED);
  while (__atomic_load_n(&lock->now_serving, __ATOMIC_ACQUIRE) != my_ticket) {
    __asm__ volatile("pause" ::: "memory");
  }
}

static inline void spinlock_release_restore(spinlock_t *lock, uint64_t flags) {
  uint16_t serving = lock->now_serving + 1;
  __atomic_store_n(&lock->now_serving, serving, __ATOMIC_RELEASE);
  hal_irq_restore((hal_irq_state_t)flags);
}

static inline bool spinlock_try_acquire(spinlock_t *lock) {
  hal_irq_state_t flags = hal_irq_save();

  uint32_t current = __atomic_load_n(&lock->val, __ATOMIC_RELAXED);
  uint16_t serving = (uint16_t)(current & 0xFFFF);
  uint16_t next = (uint16_t)(current >> 16);

  if (serving != next) {
    hal_irq_restore(flags);
    return false;
  }

  uint32_t updated = ((uint32_t)(next + 1) << 16) | serving;
  if (__atomic_compare_exchange_n(&lock->val, &current, updated, false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    lock->saved_flags = flags;
    return true;
  }

  hal_irq_restore(flags);
  return false;
}

static inline bool spinlock_is_locked(spinlock_t *lock) {
  uint32_t current = __atomic_load_n(&lock->val, __ATOMIC_RELAXED);
  uint16_t serving = (uint16_t)(current & 0xFFFF);
  uint16_t next = (uint16_t)(current >> 16);
  return serving != next;
}

#endif
