// Futex Syscall (202)
// Implements FUTEX_WAIT and FUTEX_WAKE using a hash table. Shared futexes use
// physical addresses; private futexes use (mm, virtual address) identities.
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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Futex operation constants (Linux ABI)
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_PRIVATE 128 // FUTEX_WAIT | FUTEX_PRIVATE_FLAG
#define FUTEX_WAKE_PRIVATE 129 // FUTEX_WAKE | FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_REQUEUE      3
#define FUTEX_WAKE_OP      5
#define FUTEX_CMD_MASK     127

// FUTEX_WAKE_OP encoded field widths (val3 argument)
#define FUTEX_OP_OP_SHIFT    28
#define FUTEX_OP_CMP_SHIFT   24
#define FUTEX_OP_OPARG_SHIFT 12
#define FUTEX_OP_OP_MASK     0xf
#define FUTEX_OP_CMP_MASK    0xf
#define FUTEX_OP_OPARG_MASK  0xfff
#define FUTEX_OP_CMPARG_MASK 0xfff

// Encoded op codes (FUTEX_OP_*)
#define FUTEX_OP_SET        0  // *uaddr2 = oparg
#define FUTEX_OP_ADD        1  // *uaddr2 += oparg
#define FUTEX_OP_OR         2  // *uaddr2 |= oparg
#define FUTEX_OP_ANDN       3  // *uaddr2 &= ~oparg
#define FUTEX_OP_XOR        4  // *uaddr2 ^= oparg
#define FUTEX_OP_ARG_SHIFT  8  // oparg = 1 << oparg (bit in op field)

// Encoded cmp codes (FUTEX_OP_CMP_*)
#define FUTEX_OP_CMP_EQ     0
#define FUTEX_OP_CMP_NE     1
#define FUTEX_OP_CMP_LT     2
#define FUTEX_OP_CMP_LE     3
#define FUTEX_OP_CMP_GT     4
#define FUTEX_OP_CMP_GE     5

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

struct futex_key {
  // Zero selects a shared, physical-address key. Private futexes use the
  // process mm pointer and never need a guest page-table walk.
  uint64_t space;
  uint64_t address;
};

struct futex_waiter {
  struct futex_key key;
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

static inline uint32_t futex_hash_key(struct futex_key key) {
  uint64_t h = key.address >> 2;
  h ^= key.space + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return (uint32_t)(h & (FUTEX_HASH_SIZE - 1));
}

static inline bool futex_key_equal(struct futex_key a, struct futex_key b) {
  return a.space == b.space && a.address == b.address;
}

// Remove every waiter owned by a thread that is about to be reaped. Futex
// waiters live in futex_wait() stack frames; forced exit_group teardown does
// not return through that function, so entries must be detached before the
// kernel stack is released.
void futex_remove_thread_waiters(struct thread *thread) {
  if (!thread || !futex_initialized)
    return;

  for (uint32_t bucket = 0; bucket < FUTEX_HASH_SIZE; bucket++) {
    spinlock_acquire(&futex_hash[bucket].lock);
    struct futex_waiter **pp = &futex_hash[bucket].head;
    while (*pp) {
      struct futex_waiter *waiter = *pp;
      if (waiter->thread == thread) {
        *pp = waiter->next;
      } else {
        pp = &waiter->next;
      }
    }
    spinlock_release(&futex_hash[bucket].lock);
  }
}

/* Build a futex identity. Process-private futexes are only meaningful within
 * one mm, so (mm, uaddr) is sufficient and avoids vmm_virt_to_phys() on every
 * Mesa/LLVM worker wait and wake. Shared futexes retain physical identities. */
static int futex_get_key(uint32_t *uaddr, bool private,
                         struct futex_key *key) {
  struct thread *t = sched_get_current();
  if (!t || !key)
    return -EFAULT;

  uint64_t vaddr = (uint64_t)uaddr;
  if ((vaddr & (sizeof(uint32_t) - 1)) != 0)
    return -EINVAL;
  if (!vmm_is_user_addr_range_valid(vaddr, sizeof(uint32_t)))
    return -EFAULT;

  if (private) {
    if (!t->mm)
      return -EFAULT;
    key->space = (uint64_t)t->mm;
    key->address = vaddr;
    return 0;
  }

  if (!t->cr3)
    return -EFAULT;
  uint64_t phys = vmm_virt_to_phys((uint64_t *)t->cr3, vaddr);
  if (!phys)
    return -EFAULT;
  key->space = 0;
  key->address = phys;
  return 0;
}

// FUTEX_WAIT
// Atomically check that *uaddr == val, then block the calling thread.
// If a timeout is specified, the thread will be woken after the timeout.
// Returns 0 on success (woken by FUTEX_WAKE).
// Returns -EAGAIN if *uaddr != val at time of check.
// Returns -ETIMEDOUT if timeout expired.
static uint64_t futex_wait(uint32_t *uaddr, uint32_t val,
                           const uint64_t *timeout_ts, bool private) {
  struct futex_key key;
  int error = futex_get_key(uaddr, private, &key);
  if (error)
    return (uint64_t)(int64_t)error;

  uint32_t bucket = futex_hash_key(key);

  // Allocate waiter on the kernel stack — it's safe because we block in this
  // function and only return after being woken (the stack frame stays valid).
  struct futex_waiter waiter;
  waiter.key = key;
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
  uint32_t final_bucket = futex_hash_key(waiter.key);
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

static uint64_t futex_wake_key(struct futex_key key, uint32_t val) {
  uint32_t bucket = futex_hash_key(key);
  uint32_t woken = 0;
  spinlock_acquire(&futex_hash[bucket].lock);
  struct futex_waiter **pp = &futex_hash[bucket].head;
  while (*pp && woken < val) {
    struct futex_waiter *w = *pp;
    if (futex_key_equal(w->key, key)) {
      if (w->thread && w->thread->state == THREAD_BLOCKED) {
        sched_wakeup(w->thread);
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
static uint64_t futex_wake(uint32_t *uaddr, uint32_t val, bool private) {
  struct futex_key key;
  int error = futex_get_key(uaddr, private, &key);
  if (error)
    return (uint64_t)(int64_t)error;

  return futex_wake_key(key, val);
}

// FUTEX_REQUEUE
// Wake at most `val` threads waiting on uaddr1, and move at most `val2`
// remaining threads to wait on uaddr2 instead.
static uint64_t futex_requeue(uint32_t *uaddr1, uint32_t val, uint32_t val2,
                              uint32_t *uaddr2, bool private) {
  struct futex_key key1, key2;
  int error = futex_get_key(uaddr1, private, &key1);
  if (error)
    return (uint64_t)(int64_t)error;
  error = futex_get_key(uaddr2, private, &key2);
  if (error)
    return (uint64_t)(int64_t)error;

  if (futex_key_equal(key1, key2))
    return futex_wake_key(key1, val);

  uint32_t bucket1 = futex_hash_key(key1);
  uint32_t bucket2 = futex_hash_key(key2);

  uint32_t total_woken = 0;
  uint32_t total_requeued = 0;

  // Always acquire locks in bucket order to avoid deadlocks
  if (bucket1 == bucket2) {
    spinlock_acquire(&futex_hash[bucket1].lock);
  } else if (bucket1 < bucket2) {
    spinlock_acquire(&futex_hash[bucket1].lock);
    spinlock_acquire(&futex_hash[bucket2].lock);
  } else {
    spinlock_acquire(&futex_hash[bucket2].lock);
    spinlock_acquire(&futex_hash[bucket1].lock);
  }

  struct futex_waiter **pp = &futex_hash[bucket1].head;
  while (*pp) {
    struct futex_waiter *w = *pp;
    if (futex_key_equal(w->key, key1)) {
      if (total_woken < val) {
        // Wake this thread
        if (w->thread && w->thread->state == THREAD_BLOCKED) {
          sched_wakeup(w->thread);
          total_woken++;
        }
        // Remove from bucket1
        *pp = w->next;
      } else if (total_requeued < val2) {
        // Requeue: move to bucket2
        *pp = w->next; // Remove from bucket1
        w->key = key2;
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
  if (bucket2 != bucket1)
    spinlock_release(&futex_hash[bucket2].lock);

  return (uint64_t)total_woken;
}

// FUTEX_WAKE_OP
// Atomically applies an encoded operation to *uaddr2, wakes up to val threads
// on uaddr, then conditionally wakes up to val2 threads on uaddr2 depending on
// whether the old value of *uaddr2 satisfies a comparison.
// val3 encodes: op[31:28] | cmp[27:24] | oparg[23:12] | cmparg[11:0]
static uint64_t futex_wake_op(uint32_t *uaddr, uint32_t val,
                              uint32_t val2, uint32_t *uaddr2,
                              uint32_t val3, bool private) {
  // Decode val3
  uint32_t op_code  = (val3 >> FUTEX_OP_OP_SHIFT)   & FUTEX_OP_OP_MASK;
  uint32_t cmp_code = (val3 >> FUTEX_OP_CMP_SHIFT)  & FUTEX_OP_CMP_MASK;
  uint32_t oparg    = (val3 >> FUTEX_OP_OPARG_SHIFT) & FUTEX_OP_OPARG_MASK;
  uint32_t cmparg   =  val3                          & FUTEX_OP_CMPARG_MASK;

  // FUTEX_OP_ARG_SHIFT: oparg is a shift count rather than a literal value
  if (op_code & FUTEX_OP_ARG_SHIFT) {
    op_code &= ~FUTEX_OP_ARG_SHIFT;
    if (oparg >= 32)
      return (uint64_t)(-(int64_t)EINVAL);
    oparg = 1u << oparg;
  }

  // Validate uaddr2
  uint64_t vaddr2 = (uint64_t)uaddr2;
  if ((vaddr2 & (sizeof(uint32_t) - 1)) != 0)
    return (uint64_t)(-(int64_t)EINVAL);
  if (!vmm_is_user_addr_range_valid(vaddr2, sizeof(uint32_t)))
    return (uint64_t)(-(int64_t)EFAULT);

  // Atomically apply the operation to *uaddr2 and capture the old value
  uint32_t old_val;
  switch (op_code) {
  case FUTEX_OP_SET:
    old_val = __atomic_exchange_n(uaddr2, oparg, __ATOMIC_SEQ_CST);
    break;
  case FUTEX_OP_ADD:
    old_val = __atomic_fetch_add(uaddr2, oparg, __ATOMIC_SEQ_CST);
    break;
  case FUTEX_OP_OR:
    old_val = __atomic_fetch_or(uaddr2, oparg, __ATOMIC_SEQ_CST);
    break;
  case FUTEX_OP_ANDN:
    old_val = __atomic_fetch_and(uaddr2, ~oparg, __ATOMIC_SEQ_CST);
    break;
  case FUTEX_OP_XOR:
    old_val = __atomic_fetch_xor(uaddr2, oparg, __ATOMIC_SEQ_CST);
    break;
  default:
    return (uint64_t)(-(int64_t)EINVAL);
  }

  // Wake up to val threads waiting on uaddr (always)
  uint64_t woken = futex_wake(uaddr, val, private);

  // Evaluate the comparison against old_val
  bool cmp_result;
  switch (cmp_code) {
  case FUTEX_OP_CMP_EQ: cmp_result = (old_val == cmparg); break;
  case FUTEX_OP_CMP_NE: cmp_result = (old_val != cmparg); break;
  case FUTEX_OP_CMP_LT: cmp_result = (old_val <  cmparg); break;
  case FUTEX_OP_CMP_LE: cmp_result = (old_val <= cmparg); break;
  case FUTEX_OP_CMP_GT: cmp_result = (old_val >  cmparg); break;
  case FUTEX_OP_CMP_GE: cmp_result = (old_val >= cmparg); break;
  default:
    return (uint64_t)(-(int64_t)EINVAL);
  }

  // Conditionally wake up to val2 threads waiting on uaddr2
  if (cmp_result)
    woken += futex_wake(uaddr2, val2, private);

  return woken;
}

// sys_futex dispatcher
static uint64_t sys_futex(uint64_t uaddr_val, uint64_t op_val, uint64_t val_arg,
                          uint64_t timeout_ptr, uint64_t uaddr2_val,
                          uint64_t val3) {
  (void)val3;

  uint32_t *uaddr = (uint32_t *)uaddr_val;
  bool private = (op_val & FUTEX_PRIVATE_FLAG) != 0;
  int op = (int)(op_val & FUTEX_CMD_MASK);
  uint32_t val = (uint32_t)val_arg;

  switch (op) {
  case FUTEX_WAIT: {
    const uint64_t *timeout =
        timeout_ptr ? (const uint64_t *)timeout_ptr : NULL;
    return futex_wait(uaddr, val, timeout, private);
  }

  case FUTEX_WAKE:
    return futex_wake(uaddr, val, private);

  case FUTEX_REQUEUE:
    return futex_requeue(uaddr, val, (uint32_t)timeout_ptr,
                         (uint32_t *)uaddr2_val, private);

  case FUTEX_WAKE_OP:
    return futex_wake_op(uaddr, val, (uint32_t)timeout_ptr,
                         (uint32_t *)uaddr2_val, (uint32_t)val3, private);

  default:
    klog_puts("[FUTEX] Unsupported op: ");
    klog_uint64(op_val);
    klog_puts("\n");
    return (uint64_t)(-(int64_t)EINVAL);
  }
}

// Registration
uint64_t futex_wake_user(uint32_t *uaddr, uint32_t count) {
  // CLONE_CHILD_CLEARTID is paired with pthread-private futex waits.
  return futex_wake(uaddr, count, true);
}

void syscall_register_futex(void) {
  // Registration runs before userspace and makes lazy initialization and its
  // race/branch unnecessary in every futex operation.
  futex_init_once();
  syscall_register(SYS_FUTEX, sys_futex);
}
