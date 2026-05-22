#ifndef LOCK_SPINLOCK_H
#define LOCK_SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool locked;
  unsigned long saved_flags; // Saved RFLAGS of the lock holder
} spinlock_t;

#define SPINLOCK_INIT {false, 0}

static inline void spinlock_init(spinlock_t *lock) {
  lock->locked = false;
  lock->saved_flags = 0;
}

// Interrupt-safe spinlock acquire:
// 1. Save current RFLAGS (preserving IF state)
// 2. Disable interrupts (cli) so no timer tick can preempt us
// 3. Spin until the lock is acquired
//
// Nesting works correctly:
//   acquire(A): saves IF=1, cli            → IF=0
//   acquire(B): saves IF=0, cli (noop)     → IF=0
//   release(B): restores IF=0             → IF=0
//   release(A): restores IF=1             → IF=1 (interrupts re-enabled)
static inline void spinlock_acquire(spinlock_t *lock) {
  unsigned long flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");

  while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
    __asm__ volatile("pause" ::: "memory");
  }

  lock->saved_flags = flags;
}

static inline void spinlock_release(spinlock_t *lock) {
  unsigned long flags = lock->saved_flags;
  __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
  __asm__ volatile("push %0; popfq" ::"r"(flags) : "memory");
}

static inline void spinlock_acquire_save(spinlock_t *lock, uint64_t *flags) {
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(*flags) : : "memory");
  while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
    __asm__ volatile("pause" ::: "memory");
  }
}

static inline void spinlock_release_restore(spinlock_t *lock, uint64_t flags) {
  __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
  __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

static inline bool spinlock_try_acquire(spinlock_t *lock) {
  unsigned long flags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");

  if (!__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
    lock->saved_flags = flags;
    return true;
  }

  __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
  return false;
}

#endif
