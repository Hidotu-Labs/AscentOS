#ifndef __AVORY_LINUXKPI_COMPLETION_H
#define __AVORY_LINUXKPI_COMPLETION_H

/* Linux <linux/completion.h> overlay, built on the LinuxKPI waitqueue. */

#include <linux/types.h>
#include <linux/wait.h>

struct completion {
  unsigned int done;
  struct wait_queue_head wait;
};

#define COMPLETION_INITIALIZER(work)                                          \
  {                                                                           \
    0, __WAIT_QUEUE_HEAD_INITIALIZER((work).wait)                             \
  }
#define DECLARE_COMPLETION(work) struct completion work = COMPLETION_INITIALIZER(work)
#define DECLARE_COMPLETION_ONSTACK(work) DECLARE_COMPLETION(work)

#define init_completion(x)                                                    \
  do {                                                                        \
    (x)->done = 0;                                                            \
    init_waitqueue_head(&(x)->wait);                                          \
  } while (0)
#define reinit_completion(x) ((x)->done = 0)

void complete(struct completion *x);
void complete_all(struct completion *x);
void wait_for_completion(struct completion *x);
long wait_for_completion_interruptible(struct completion *x);
long wait_for_completion_killable(struct completion *x);
unsigned long wait_for_completion_timeout(struct completion *x,
                                          unsigned long timeout);
long wait_for_completion_interruptible_timeout(struct completion *x,
                                                unsigned long timeout);
bool try_wait_for_completion(struct completion *x);
bool completion_done(struct completion *x);

#endif /* __AVORY_LINUXKPI_COMPLETION_H */
