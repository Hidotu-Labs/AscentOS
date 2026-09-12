#ifndef __AVORY_LINUXKPI_SCHED_H
#define __AVORY_LINUXKPI_SCHED_H

/* Minimal Linux <linux/sched.h> overlay.
 *
 * `current` is an opaque handle: it is the native thread pointer cast to
 * struct task_struct*.  This is enough for sleeping/waking and for passing
 * "the current task" around; fields (comm/pid) arrive with the real task
 * model.  The upstream header cannot be used here yet (per-CPU/thread_info,
 * scheduling classes, mm). */

#include <linux/limits.h>
#include <linux/preempt.h>
#include <linux/types.h>

#include <linuxkpi/native_sched.h>

struct task_struct; /* opaque: a native thread handle */

#define current ((struct task_struct *)linuxkpi_current_thread())

#define TASK_RUNNING 0x00000000
#define TASK_INTERRUPTIBLE 0x00000001
#define TASK_UNINTERRUPTIBLE 0x00000002
#define TASK_STOPPED 0x00000004
#define TASK_TRACED 0x00000008
#define TASK_WAKEKILL 0x00000080
#define TASK_KILLABLE (TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)

#define MAX_SCHEDULE_TIMEOUT LONG_MAX

/* State changes are implicit in the sleep helpers (the task model is not
 * exported yet); keep the API shape so upstream code compiles. */
#define set_current_state(state_value)                                        \
  do {                                                                        \
    (void)(state_value);                                                      \
  } while (0)
#define __set_current_state(state_value) set_current_state(state_value)

void schedule(void);
long schedule_timeout(long timeout);
long schedule_timeout_interruptible(long timeout);
long schedule_timeout_uninterruptible(long timeout);
long schedule_timeout_killable(long timeout);

int wake_up_process(struct task_struct *p);
bool signal_pending(struct task_struct *p);
void yield(void);

#endif /* __AVORY_LINUXKPI_SCHED_H */
