/* LinuxKPI waitqueues.  See linux/wait.h for the contract. */

#include <linux/wait.h>

#include <linuxkpi/native_sched.h>

void init_wait_entry(struct wait_queue_entry *wq_entry, int flags) {
  wq_entry->flags = (unsigned int)flags;
  wq_entry->private = linuxkpi_current_thread();
  wq_entry->func = autoremove_wake_function;
  INIT_LIST_HEAD(&wq_entry->entry);
}

int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
                          int flags, void *key) {
  (void)mode;
  (void)flags;
  (void)key;
  linuxkpi_wake_thread(wq_entry->private);
  return 1;
}

int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
                             int flags, void *key) {
  int ret = default_wake_function(wq_entry, mode, flags, key);

  if (ret)
    list_del_init(&wq_entry->entry);
  return ret;
}

void add_wait_queue(wait_queue_head_t *q, struct wait_queue_entry *wq_entry) {
  unsigned long flags;

  spin_lock_irqsave(&q->lock, flags);
  if (list_empty(&wq_entry->entry))
    list_add(&wq_entry->entry, &q->head);
  spin_unlock_irqrestore(&q->lock, flags);
}

void remove_wait_queue(wait_queue_head_t *q,
                       struct wait_queue_entry *wq_entry) {
  unsigned long flags;

  spin_lock_irqsave(&q->lock, flags);
  list_del_init(&wq_entry->entry);
  spin_unlock_irqrestore(&q->lock, flags);
}

void prepare_to_wait(wait_queue_head_t *q, struct wait_queue_entry *wq_entry,
                     int state) {
  unsigned long flags;

  (void)state;
  spin_lock_irqsave(&q->lock, flags);
  if (list_empty(&wq_entry->entry))
    list_add(&wq_entry->entry, &q->head);
  spin_unlock_irqrestore(&q->lock, flags);
}

int prepare_to_wait_event(wait_queue_head_t *q,
                          struct wait_queue_entry *wq_entry, int state) {
  unsigned long flags;

  spin_lock_irqsave(&q->lock, flags);
  if (list_empty(&wq_entry->entry))
    list_add(&wq_entry->entry, &q->head);
  spin_unlock_irqrestore(&q->lock, flags);

  if (___wait_is_interruptible(state) && signal_pending(current))
    return -ERESTARTSYS;
  return 0;
}

void finish_wait(wait_queue_head_t *q, struct wait_queue_entry *wq_entry) {
  unsigned long flags;

  spin_lock_irqsave(&q->lock, flags);
  list_del_init(&wq_entry->entry);
  spin_unlock_irqrestore(&q->lock, flags);
}

int __kpi_wake_up(wait_queue_head_t *q, unsigned int nr, int flags) {
  unsigned long irqflags;
  int woken = 0;

  /* Wake functions must run under the queue lock.  A waiter that times out or
   * is interrupted can otherwise finish_wait() and return from the enclosing
   * wait_event() - reusing its stack-allocated entry - while this loop is
   * still calling into it.  Entries are unlinked before the call so the walk
   * stays valid even for wake functions that leave the entry queued. */
  spin_lock_irqsave(&q->lock, irqflags);
  while (!list_empty(&q->head)) {
    struct wait_queue_entry *wq_entry =
        list_first_entry(&q->head, struct wait_queue_entry, entry);
    list_del_init(&wq_entry->entry);
    wq_entry->func(wq_entry, 0, flags, NULL);
    woken++;
    if (nr && (unsigned int)woken >= nr)
      break;
  }
  spin_unlock_irqrestore(&q->lock, irqflags);

  return woken;
}
