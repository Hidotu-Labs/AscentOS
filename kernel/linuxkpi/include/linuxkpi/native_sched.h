#ifndef LINUXKPI_NATIVE_SCHED_H
#define LINUXKPI_NATIVE_SCHED_H

/* Native scheduler / time bridge for LinuxKPI implementation files.
 * Implementations: kernel/src/linuxkpi/native_sched.c (native headers only).
 *
 * No stdint/stddef typedefs are used here so the header cannot conflict with
 * the Linux headers the implementation files also include. */

/* The calling thread's native `struct thread *`, as an opaque handle. */
void *linuxkpi_current_thread(void);

/* Create and enqueue a kernel thread with an argument.  Returns the opaque
 * thread handle, or 0 on failure.  `name` is truncated to 15 characters. */
void *linuxkpi_kthread_create(void (*fn)(void *), void *arg, const char *name);

/* Block the calling thread with no timeout; woken by linuxkpi_wake_thread(). */
void linuxkpi_thread_block(void);

/* Block the calling thread for up to `ms` milliseconds.
 * Returns 1 if woken early and 0 if the timeout expired. */
int linuxkpi_schedule_timeout_ms(unsigned long ms);

/* Wake a thread previously blocked by the helpers above. */
void linuxkpi_wake_thread(void *thread);

/* True if the thread has a pending signal. */
_Bool linuxkpi_thread_has_pending_signal(void *thread);

/* Monotonic time since boot. */
unsigned long long linuxkpi_monotonic_ms(void);
unsigned long long linuxkpi_monotonic_ns(void);

/* Number of online CPUs. */
int linuxkpi_cpu_count(void);

/* Busy-wait for `ns` nanoseconds (TSC based). */
void linuxkpi_udelay_ns(unsigned long long ns);

/* Per-thread LinuxKPI control block attached to a native thread (see
 * kernel/src/sched/sched.h kpi_data).  `linuxkpi_thread_self_data` reads the
 * calling thread's block. */
void *linuxkpi_thread_data(void *thread);
void *linuxkpi_thread_self_data(void);

#endif /* LINUXKPI_NATIVE_SCHED_H */
