/* Native scheduler/time bridge for the LinuxKPI sleeping layer.  Compiled with
 * native headers only; see linuxkpi/native_sched.h for the contract. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "hal/hal.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "smp/cpu.h"

struct linuxkpi_kthread_boot {
  void (*fn)(void *);
  void *arg;
};

/* LinuxKPI time layer (linuxkpi/src/time.c) when the Linux tree is imported;
 * absent in a kernel built without it. */
extern void linuxkpi_jiffies_sync(void) __attribute__((weak));

void *linuxkpi_current_thread(void) { return sched_get_current(); }

void linuxkpi_yield(void) { sched_yield(); }

/* sched_get_current() is only valid once the per-CPU GS base is installed;
 * the IRQ hooks run from the first timer interrupt, which can precede
 * cpu_init(). */
static struct thread *kpi_current_thread_or_null(void) {
  struct cpu_info *cpu = cpu_get_current();
  return cpu ? cpu->current_thread : NULL;
}

bool linuxkpi_need_resched(void) {
  struct thread *t = kpi_current_thread_or_null();
  return t ? __atomic_load_n(&t->need_resched, __ATOMIC_ACQUIRE) : false;
}

/* ── hardirq context tracking (in_interrupt() backing) ──────────────────── */

/* The native ISR calls these around hardware-IRQ dispatch (src/cpu/isr.c).
 * Depth is tracked on the interrupted thread, so a different thread scheduled
 * in from inside the handler sees depth 0; when the handler's context resumes
 * on its original thread, the matching exit runs there too. */
void linuxkpi_irq_enter(void) {
  struct thread *t = kpi_current_thread_or_null();
  if (t)
    __atomic_add_fetch(&t->kpi_irq_depth, 1, __ATOMIC_ACQ_REL);
}

void linuxkpi_irq_exit(void) {
  struct thread *t = kpi_current_thread_or_null();
  if (t && __atomic_load_n(&t->kpi_irq_depth, __ATOMIC_RELAXED))
    __atomic_sub_fetch(&t->kpi_irq_depth, 1, __ATOMIC_ACQ_REL);
}

int linuxkpi_irq_depth(void) {
  struct thread *t = kpi_current_thread_or_null();
  return t ? (int)__atomic_load_n(&t->kpi_irq_depth, __ATOMIC_RELAXED) : 0;
}

int linuxkpi_softirq_depth(void) {
  struct thread *t = kpi_current_thread_or_null();
  return t ? (int)__atomic_load_n(&t->kpi_softirq_depth, __ATOMIC_RELAXED) : 0;
}

static void linuxkpi_kthread_trampoline(void) {
  struct thread *t = sched_get_current();
  struct linuxkpi_kthread_boot *boot =
      t ? (struct linuxkpi_kthread_boot *)t->kpi_data : NULL;

  if (!boot)
    return;

  /* Hand the thread its long-lived argument (the LinuxKPI kthread control
   * block) before invoking the entry point, then free the start record. */
  void (*fn)(void *) = boot->fn;
  void *arg = boot->arg;

  if (t)
    t->kpi_data = arg;
  kfree(boot);

  fn(arg);
}

void *linuxkpi_kthread_create(void (*fn)(void *), void *arg, const char *name) {
  struct linuxkpi_kthread_boot *boot = kmalloc(sizeof(*boot));
  if (!boot)
    return NULL;

  boot->fn = fn;
  boot->arg = arg;

  /* Create suspended so kpi_data/comm are set before it can run. */
  struct thread *t =
      sched_create_kernel_thread(linuxkpi_kthread_trampoline, NULL, false);
  if (!t) {
    kfree(boot);
    return NULL;
  }

  t->kpi_data = boot;
  if (name) {
    strncpy(t->comm, name, sizeof(t->comm) - 1);
    t->comm[sizeof(t->comm) - 1] = '\0';
  }

  sched_enqueue_thread(t, NULL);
  return t;
}

void linuxkpi_thread_block(void) {
  struct thread *t = sched_get_current();
  if (!t)
    return;
  t->state = THREAD_BLOCKED;
  sched_yield();
}

int linuxkpi_schedule_timeout_ms(unsigned long ms) {
  struct thread *t = sched_get_current();
  if (!t)
    return 0;

  if (ms == 0) {
    sched_yield();
    return 0;
  }

  uint64_t deadline = lapic_timer_get_ticks() + (uint64_t)ms;
  t->wakeup_ticks = deadline;

  /* Sleep until the deadline, not just for one scheduler pass.
   *
   * The native scheduler can hand the CPU back before the deadline when the
   * caller is a per-CPU idle task: sched_schedule() treats cpu->idle_thread as
   * the always-available fallback and resumes it regardless of its state or
   * wakeup_ticks.  kmain_high_half runs in exactly that context before the
   * first real kernel thread is scheduled, so a single yield would return
   * immediately and every msleep()/schedule_timeout() would be a no-op.
   *
   * A genuine wakeup (linuxkpi_wake_thread() -> sched_wakeup()) clears
   * wakeup_ticks, which also cancels the sleep here; otherwise the loop
   * re-blocks until the clock reaches the deadline. */
  while (lapic_timer_get_ticks() < deadline) {
    t->state = THREAD_SLEEPING;
    sched_yield();
    if (!t->wakeup_ticks)
      break;
  }

  bool timed_out = lapic_timer_get_ticks() >= deadline;
  t->wakeup_ticks = 0;
  /* Timed-out sleeping calls can leave the state as SLEEPING when the idle
   * fallback never switched away; the caller is definitely running now. */
  if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED)
    t->state = THREAD_RUNNING;

  /* jiffies advances with the native tick, which may not have fired between
   * the last sync and this wakeup (the LAPIC is one-shot and armed at the
   * next scheduler deadline).  Bring it current so msleep()/schedule_timeout()
   * callers observe the interval they slept. */
  if (linuxkpi_jiffies_sync)
    linuxkpi_jiffies_sync();

  return timed_out ? 0 : 1;
}

void linuxkpi_wake_thread(void *thread) {
  struct thread *t = (struct thread *)thread;
  if (!t)
    return;

  /* sched_wakeup() ignores idle threads: they are the per-CPU fallback task
   * and are never runqueue members.  KPI code can still sleep in the boot
   * thread's idle context before the first real kernel thread is scheduled
   * (e.g. the bounded wait in linuxkpi_run_boot_tests()), so cancel the
   * timeout here instead.  The scheduler resumes the idle fallback as soon as
   * nothing else is runnable, and the sleep loop then sees wakeup_ticks==0
   * and returns. */
  if (t->is_idle) {
    t->wakeup_ticks = 0;
    if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED)
      __atomic_store_n(&t->state, THREAD_RUNNING, __ATOMIC_RELEASE);
    return;
  }

  sched_wakeup(t);
}

bool linuxkpi_thread_has_pending_signal(void *thread) {
  struct thread *t = (struct thread *)thread;
  return t ? thread_has_pending_signal(t) : false;
}

unsigned long long linuxkpi_monotonic_ms(void) { return lapic_timer_get_ms(); }

unsigned long long linuxkpi_monotonic_ns(void) { return lapic_timer_get_ns(); }

int linuxkpi_cpu_count(void) { return (int)cpu_get_count(); }

void linuxkpi_udelay_ns(unsigned long long ns) {
  unsigned long long start = lapic_timer_get_ns();
  while (lapic_timer_get_ns() - start < ns)
    hal_cpu_relax();
}

void *linuxkpi_thread_data(void *thread) {
  struct thread *t = (struct thread *)thread;
  return t ? t->kpi_data : NULL;
}

void *linuxkpi_thread_self_data(void) {
  struct thread *t = sched_get_current();
  return t ? t->kpi_data : NULL;
}
