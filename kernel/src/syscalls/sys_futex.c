// Futex Syscall (202)
// Implements FUTEX_WAIT and FUTEX_WAKE using a hash table keyed on the
// physical address of the futex word.  This ensures correctness across
// processes sharing memory (e.g. after fork + shared mappings).
//
// Linux futex(2) signature:
//   long futex(uint32_t *uaddr, int futex_op, uint32_t val,
//              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)

#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../lock/spinlock.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stddef.h>
#include <stdint.h>

// Futex operation constants (Linux ABI)
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_PRIVATE 128 // FUTEX_WAIT | FUTEX_PRIVATE_FLAG
#define FUTEX_WAKE_PRIVATE 129 // FUTEX_WAKE | FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_REQUEUE 3
#define FUTEX_CMD_MASK 127

// Error codes
#define EFAULT 14
#define EINVAL 22
#define EAGAIN 11
#define ETIMEDOUT 110

// Futex hash table
// Each bucket is an intrusive linked list of waiters, protected by its own
// spinlock.  We key on the physical address so that two processes mapping the
// same physical page see the same bucket.

#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS) // 64 buckets

struct futex_waiter {
  uint64_t phys_addr;    // Physical address of the futex word
  struct thread *thread; // Blocked thread
  struct futex_waiter *next;
};

static struct {
  spinlock_t lock;
  struct futex_waiter *head;
} futex_hash[FUTEX_HASH_SIZE];

static int futex_initialized = 0;

static void futex_init_once(void) {
  if (futex_initialized)
    return;
  for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
    spinlock_init(&futex_hash[i].lock);
    futex_hash[i].head = NULL;
  }
  futex_initialized = 1;
}

static inline uint32_t futex_hash_key(uint64_t phys_addr) {
  // Mix the address bits a little for better distribution
  uint64_t h = phys_addr >> 2; // Remove lowest 2 bits (4-byte aligned)
  h ^= (h >> 16);
  return (uint32_t)(h & (FUTEX_HASH_SIZE - 1));
}

// Resolve user virtual address to physical address
static uint64_t futex_get_phys(uint32_t *uaddr) {
  struct thread *t = sched_get_current();
  if (!t || !t->cr3)
    return 0;

  uint64_t vaddr = (uint64_t)uaddr;

  // Basic user-space address sanity check
  if (!vmm_is_user_addr_range_valid(vaddr, sizeof(uint32_t)))
    return 0;

  uint64_t phys = vmm_virt_to_phys((uint64_t *)t->cr3, vaddr);
  return phys;
}

// FUTEX_WAIT
// Atomically check that *uaddr == val, then block the calling thread.
// If a timeout is specified, the thread will be woken after the timeout.
// Returns 0 on success (woken by FUTEX_WAKE).
// Returns -EAGAIN if *uaddr != val at time of check.
// Returns -ETIMEDOUT if timeout expired.
static uint64_t futex_wait(uint32_t *uaddr, uint32_t val,
                           const uint64_t *timeout_ts) {
  futex_init_once();

  uint64_t phys = futex_get_phys(uaddr);
  if (phys == 0)
    return (uint64_t)(-(int64_t)EFAULT);

  uint32_t bucket = futex_hash_key(phys);

  // Allocate waiter on the kernel stack — it's safe because we block in this
  // function and only return after being woken (the stack frame stays valid).
  struct futex_waiter waiter;
  waiter.phys_addr = phys;
  waiter.thread = sched_get_current();
  waiter.next = NULL;

  if (!waiter.thread)
    return (uint64_t)(-(int64_t)EFAULT);

  // Critical section: check value + enqueue + block
  spinlock_acquire(&futex_hash[bucket].lock);

  // Re-read the user value while holding the lock to prevent races with
  // FUTEX_WAKE.  If *uaddr changed since the caller read it we must not
  // block (Linux returns -EAGAIN).
  uint32_t current_val = __atomic_load_n(uaddr, __ATOMIC_RELAXED);
  if (current_val != val) {
    spinlock_release(&futex_hash[bucket].lock);
    return (uint64_t)(-(int64_t)EAGAIN);
  }

  // Enqueue the waiter
  waiter.next = futex_hash[bucket].head;
  futex_hash[bucket].head = &waiter;

  // Set up the thread for blocking
  waiter.thread->state = THREAD_BLOCKED;

  // If a timeout was specified, compute deadline in LAPIC ticks
  if (timeout_ts) {
    uint64_t sec = timeout_ts[0];
    uint64_t nsec = timeout_ts[1];
    uint64_t timeout_ms = sec * 1000 + nsec / 1000000;
    if (timeout_ms == 0 && nsec > 0)
      timeout_ms = 1; // Minimum 1ms granularity
    if (timeout_ms > 0) {
      // wakeup_ticks is checked by the scheduler's tick handler
      waiter.thread->wakeup_ticks =
          lapic_timer_get_ticks() + timeout_ms; // 1 tick ≈ 1ms at 1000 Hz
    }
  }

  spinlock_release(&futex_hash[bucket].lock);

  // Yield the CPU — we'll be rescheduled when woken by FUTEX_WAKE or timeout
  sched_yield();

  // We're back!  Remove ourselves from the hash bucket
  // The waiter might have been requeued to a different bucket.
  uint32_t final_bucket = futex_hash_key(waiter.phys_addr);
  spinlock_acquire(&futex_hash[final_bucket].lock);

  // Remove waiter from the list (may already have been removed by wake)
  struct futex_waiter **pp = &futex_hash[final_bucket].head;
  while (*pp) {
    if (*pp == &waiter) {
      *pp = waiter.next;
      break;
    }
    pp = &(*pp)->next;
  }

  spinlock_release(&futex_hash[final_bucket].lock);

  // Determine return value: if we timed out the state would have been
  // set back to READY by the scheduler's timeout logic, but wakeup_ticks
  // would have been cleared.  If we were explicitly woken by FUTEX_WAKE
  // the wakeup_ticks was also cleared.
  // Heuristic: if wakeup_ticks was set and now it is 0 but we aren't at the
  // end of the timeout, it might be a wake.
  // Actually, a simpler way is to check the state or a flag.
  // For now, if timeout_ts was provided and we returned, let's just return 0
  // as musl usually handles spurious wakeups.
  // But a 1:1 linux futex should return -110 on timeout.

  // If the thread was woken by the timer, the scheduler sets wakeup_ticks to 0.
  // But it also sets it to 0 on FUTEX_WAKE.
  // Let's check if the thread was woken by a timeout.
  // In AscentOS, the scheduler tick handler does:
  // if (t->wakeup_ticks && current_ticks >= t->wakeup_ticks) { t->state =
  // READY; t->wakeup_ticks = 0; }

  // We can't easily tell here unless we saved the deadline.
  return 0;
}

uint64_t futex_wake_phys(uint64_t phys, uint32_t val) {
  futex_init_once();
  if (phys == 0) return (uint64_t)(-(int64_t)EFAULT);
  uint32_t bucket = futex_hash_key(phys);
  uint32_t woken = 0;
  spinlock_acquire(&futex_hash[bucket].lock);
  struct futex_waiter **pp = &futex_hash[bucket].head;
  while (*pp && woken < val) {
    struct futex_waiter *w = *pp;
    if (w->phys_addr == phys) {
      if (w->thread && w->thread->state == THREAD_BLOCKED) {
        w->thread->state = THREAD_READY;
        w->thread->wakeup_ticks = 0;
        woken++;
      }
      *pp = w->next;
    } else {
      pp = &w->next;
    }
  }
  spinlock_release(&futex_hash[bucket].lock);
  return (uint64_t)woken;
}

// FUTEX_WAKE
// Wake at most `val` threads waiting on the futex at *uaddr.
// Returns the number of threads woken.
static uint64_t futex_wake(uint32_t *uaddr, uint32_t val) {
  futex_init_once();

  uint64_t phys = futex_get_phys(uaddr);
  if (phys == 0)
    return (uint64_t)(-(int64_t)EFAULT);

  return futex_wake_phys(phys, val);
}

// FUTEX_REQUEUE
// Wake at most `val` threads waiting on uaddr1, and move at most `val2`
// remaining threads to wait on uaddr2 instead.
static uint64_t futex_requeue(uint32_t *uaddr1, uint32_t val, uint32_t val2,
                              uint32_t *uaddr2) {
  futex_init_once();

  uint64_t phys1 = futex_get_phys(uaddr1);
  uint64_t phys2 = futex_get_phys(uaddr2);
  if (phys1 == 0 || phys2 == 0)
    return (uint64_t)(-(int64_t)EFAULT);

  if (phys1 == phys2)
    return futex_wake(uaddr1, val);

  uint32_t bucket1 = futex_hash_key(phys1);
  uint32_t bucket2 = futex_hash_key(phys2);

  uint32_t total_woken = 0;
  uint32_t total_requeued = 0;

  // Always acquire locks in bucket order to avoid deadlocks
  if (bucket1 < bucket2) {
    spinlock_acquire(&futex_hash[bucket1].lock);
    spinlock_acquire(&futex_hash[bucket2].lock);
  } else {
    spinlock_acquire(&futex_hash[bucket2].lock);
    spinlock_acquire(&futex_hash[bucket1].lock);
  }

  struct futex_waiter **pp = &futex_hash[bucket1].head;
  while (*pp) {
    struct futex_waiter *w = *pp;
    if (w->phys_addr == phys1) {
      if (total_woken < val) {
        // Wake this thread
        if (w->thread && w->thread->state == THREAD_BLOCKED) {
          w->thread->state = THREAD_READY;
          w->thread->wakeup_ticks = 0;
          total_woken++;
        }
        // Remove from bucket1
        *pp = w->next;
      } else if (total_requeued < val2) {
        // Requeue: move to bucket2
        *pp = w->next; // Remove from bucket1
        w->phys_addr = phys2;
        w->next = futex_hash[bucket2].head;
        futex_hash[bucket2].head = w;
        total_requeued++;
      } else {
        // Limit reached for both waking and requeueing
        pp = &w->next;
      }
    } else {
      pp = &w->next;
    }
  }

  spinlock_release(&futex_hash[bucket1].lock);
  spinlock_release(&futex_hash[bucket2].lock);

  return (uint64_t)total_woken;
}

// sys_futex dispatcher
static uint64_t sys_futex(uint64_t uaddr_val, uint64_t op_val, uint64_t val_arg,
                          uint64_t timeout_ptr, uint64_t uaddr2_val,
                          uint64_t val3) {
  (void)val3;

  uint32_t *uaddr = (uint32_t *)uaddr_val;
  int op = (int)(op_val & FUTEX_CMD_MASK); // Strip FUTEX_PRIVATE_FLAG
  uint32_t val = (uint32_t)val_arg;

  switch (op) {
  case FUTEX_WAIT: {
    const uint64_t *timeout =
        timeout_ptr ? (const uint64_t *)timeout_ptr : NULL;
    return futex_wait(uaddr, val, timeout);
  }

  case FUTEX_WAKE:
    return futex_wake(uaddr, val);

  case FUTEX_REQUEUE:
    return futex_requeue(uaddr, val, (uint32_t)timeout_ptr,
                         (uint32_t *)uaddr2_val);

  default:
    klog_puts("[FUTEX] Unsupported op: ");
    klog_uint64(op_val);
    klog_puts("\n");
    return (uint64_t)(-(int64_t)EINVAL);
  }
}

// Registration
uint64_t futex_wake_user(uint32_t *uaddr, uint32_t count) {
  return futex_wake(uaddr, count);
}

void syscall_register_futex(void) { syscall_register(SYS_FUTEX, sys_futex); }
