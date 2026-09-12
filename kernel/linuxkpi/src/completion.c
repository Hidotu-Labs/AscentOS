/* LinuxKPI completions.  See linux/completion.h for the contract. */

#include <linux/completion.h>

#define UINT_MAX_DONE (~0U)

static inline void completion_account(struct completion *x) {
  unsigned long flags;

  spin_lock_irqsave(&x->wait.lock, flags);
  if (x->done != UINT_MAX_DONE)
    x->done--;
  spin_unlock_irqrestore(&x->wait.lock, flags);
}

static inline bool completion_done_locked(struct completion *x) {
  return __atomic_load_n(&x->done, __ATOMIC_ACQUIRE) != 0;
}

void complete(struct completion *x) {
  unsigned long flags;

  spin_lock_irqsave(&x->wait.lock, flags);
  if (x->done != UINT_MAX_DONE)
    x->done++;
  spin_unlock_irqrestore(&x->wait.lock, flags);

  __kpi_wake_up(&x->wait, 1, 0);
}

void complete_all(struct completion *x) {
  unsigned long flags;

  spin_lock_irqsave(&x->wait.lock, flags);
  x->done = UINT_MAX_DONE;
  spin_unlock_irqrestore(&x->wait.lock, flags);

  __kpi_wake_up(&x->wait, 0, 0);
}

void wait_for_completion(struct completion *x) {
  if (!completion_done_locked(x))
    wait_event(x->wait, completion_done_locked(x));
  completion_account(x);
}

long wait_for_completion_interruptible(struct completion *x) {
  if (!completion_done_locked(x)) {
    long ret = wait_event_interruptible(x->wait, completion_done_locked(x));
    if (ret)
      return ret;
  }
  completion_account(x);
  return 0;
}

long wait_for_completion_killable(struct completion *x) {
  if (!completion_done_locked(x)) {
    long ret = wait_event_killable(x->wait, completion_done_locked(x));
    if (ret)
      return ret;
  }
  completion_account(x);
  return 0;
}

unsigned long wait_for_completion_timeout(struct completion *x,
                                          unsigned long timeout) {
  unsigned long ret = timeout;

  if (!completion_done_locked(x))
    ret = (unsigned long)wait_event_timeout(x->wait, completion_done_locked(x),
                                            timeout);
  if (ret > 0)
    completion_account(x);
  return ret;
}

long wait_for_completion_interruptible_timeout(struct completion *x,
                                               unsigned long timeout) {
  long ret = (long)timeout;

  if (!completion_done_locked(x))
    ret = wait_event_interruptible_timeout(x->wait, completion_done_locked(x),
                                           timeout);
  if (ret > 0)
    completion_account(x);
  return ret;
}

bool try_wait_for_completion(struct completion *x) {
  unsigned long flags;
  bool ret = false;

  spin_lock_irqsave(&x->wait.lock, flags);
  if (x->done != 0) {
    if (x->done != UINT_MAX_DONE)
      x->done--;
    ret = true;
  }
  spin_unlock_irqrestore(&x->wait.lock, flags);

  return ret;
}

bool completion_done(struct completion *x) {
  return completion_done_locked(x);
}
