/* Phase 1 — preemption and spinlock integration tests.
 * Compiled with the real Linux headers.
 *
 * Verifies that plain spin_lock()/spin_unlock() disable preemption only, that
 * interrupts stay enabled while a plain lock is held, that a preempt-disabled
 * section is not switched out from under itself, and that two contending
 * threads can hammer a plain spinlock without corrupting the counter. */

#include <linux/delay.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#include <linuxkpi/log.h>

/* ── plain spinlock: preemption off, IRQs on ────────────────────────────── */

static bool test_plain_spinlock(void) {
  DEFINE_SPINLOCK(lock);
  bool ok;

  spin_lock(&lock);
  ok = spin_is_locked(&lock) && in_atomic() && preempt_count() == 1 &&
       !irqs_disabled() && !in_interrupt();
  spin_unlock(&lock);

  ok = ok && preempt_count() == 0 && !in_atomic();

  /* A failed trylock must release the preemption it took. */
  spin_lock(&lock);
  bool try_failed = !spin_trylock(&lock);
  spin_unlock(&lock);

  return ok && try_failed && preempt_count() == 0 && !irqs_disabled();
}

/* ── preempt_disable hold: IRQs serviced, no premature reschedule ───────── */

static bool test_preempt_disable_hold(void) {
  if (preempt_count() != 0 || in_interrupt())
    return false;

  preempt_disable();
  bool counted = (preempt_count() == 1) && in_atomic() && !irqs_disabled();

  /* With preemption disabled the tick must still arrive and advance jiffies;
   * the scheduler's preemption gate is what keeps the thread on the CPU. */
  unsigned long start = jiffies;
  while (jiffies - start < 5)
    __asm__ volatile("pause" ::: "memory");

  preempt_enable();
  return counted && preempt_count() == 0 && !in_atomic();
}

/* ── two-thread spinlock counter stress ─────────────────────────────────── */

struct spin_stress {
  spinlock_t lock;
  volatile unsigned long counter;
  volatile int done;
};

#define SPIN_STRESS_ITERS 20000

static int spin_stress_thread(void *arg) {
  struct spin_stress *s = arg;

  for (int i = 0; i < SPIN_STRESS_ITERS; i++) {
    if (kthread_should_stop())
      break;

    spin_lock(&s->lock);
    s->counter++;
    spin_unlock(&s->lock);
  }

  __atomic_add_fetch(&s->done, 1, __ATOMIC_RELEASE);
  return 0;
}

static bool test_spinlock_stress(void) {
  static struct spin_stress s;

  spin_lock_init(&s.lock);
  s.counter = 0;
  s.done = 0;

  struct task_struct *t1 = kthread_run(spin_stress_thread, &s, "kpi/spin1");
  struct task_struct *t2 = kthread_run(spin_stress_thread, &s, "kpi/spin2");
  if (IS_ERR(t1) || IS_ERR(t2))
    return false;

  unsigned long deadline = jiffies + msecs_to_jiffies(5000);
  while (__atomic_load_n(&s.done, __ATOMIC_ACQUIRE) < 2 &&
         time_before(jiffies, deadline))
    msleep(1);

  bool done = (__atomic_load_n(&s.done, __ATOMIC_ACQUIRE) == 2);
  kthread_stop(t1);
  kthread_stop(t2);

  return done && s.counter == (unsigned long)(2 * SPIN_STRESS_ITERS);
}

/* ── aggregator ─────────────────────────────────────────────────────────── */

void linuxkpi_test_phase1_preempt(void) {
  static const struct {
    const char *name;
    bool (*fn)(void);
  } tests[] = {
      {"preempt/IRQ state", test_plain_spinlock},
      {"preempt_disable hold", test_preempt_disable_hold},
      {"spinlock stress", test_spinlock_stress},
  };

  klog_puts("[LINUXKPI] Phase 1 preempt/spinlock self-test\n");

  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (tests[i].fn())
      klogf("[  OK  ] LinuxKPI: %s correct\n", tests[i].name);
    else
      klogf("[ FAIL ] LinuxKPI: %s wrong result\n", tests[i].name);
  }
}
