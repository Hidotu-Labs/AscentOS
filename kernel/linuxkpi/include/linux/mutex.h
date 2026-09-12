#ifndef __AVORY_LINUXKPI_MUTEX_H
#define __AVORY_LINUXKPI_MUTEX_H

/* Linux <linux/mutex.h> overlay: sleeping mutex with FIFO handoff.
 *
 * A waiter that cannot take the lock queues itself, blocks on the native
 * scheduler, and is woken by the unlocking thread, which hands ownership over
 * directly (no thundering herd, no lost wakeups).  Implementations:
 * linuxkpi/src/mutex.c. */

#include <linux/list.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct mutex {
  spinlock_t wait_lock;
  struct list_head wait_list;
  struct task_struct *owner;
};

#define __MUTEX_INITIALIZER(lockname)                                         \
  {{0, 0}, LIST_HEAD_INIT((lockname).wait_list), NULL}
#define DEFINE_MUTEX(mutexname)                                               \
  struct mutex mutexname = __MUTEX_INITIALIZER(mutexname)

void mutex_init(struct mutex *lock);
void mutex_lock(struct mutex *lock);
int mutex_lock_interruptible(struct mutex *lock);
int mutex_lock_killable(struct mutex *lock);
void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
int mutex_trylock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);
bool mutex_is_locked(struct mutex *lock);
void mutex_destroy(struct mutex *lock);

#endif /* __AVORY_LINUXKPI_MUTEX_H */
