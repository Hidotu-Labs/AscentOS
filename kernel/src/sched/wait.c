#include "wait.h"
#include "hal/hal.h"
#include "../mm/heap.h"
#include "sched.h"
#include <stddef.h>

void wait_queue_init(wait_queue_t *wq) {
  if (!wq)
    return;
  spinlock_init(&wq->lock);
  wq->head = NULL;
}

void wait_queue_add(wait_queue_t *wq, wait_queue_entry_t *entry) {
  if (!wq || !entry)
    return;

  entry->wq = wq;
  struct thread *t = entry->thread;

  spinlock_acquire(&wq->lock);
  entry->next = wq->head;
  wq->head = entry;
  spinlock_release(&wq->lock);

  if (t) {
    spinlock_acquire(&t->wait_entries_lock);
    entry->thread_next = t->wait_entries_head;
    entry->thread_prev = NULL;
    if (t->wait_entries_head)
      t->wait_entries_head->thread_prev = entry;
    t->wait_entries_head = entry;
    spinlock_release(&t->wait_entries_lock);
  }
}

void wait_queue_remove(wait_queue_t *wq, wait_queue_entry_t *entry) {
  if (!entry)
    return;

  if (!wq)
    wq = entry->wq;

  if (wq) {
    spinlock_acquire(&wq->lock);
    if (wq->head == entry) {
      wq->head = entry->next;
    } else {
      wait_queue_entry_t *curr = wq->head;
      while (curr && curr->next != entry) {
        curr = curr->next;
      }
      if (curr) {
        curr->next = entry->next;
      }
    }
    entry->next = NULL;
    entry->wq = NULL;
    spinlock_release(&wq->lock);
  }

  struct thread *t = entry->thread;
  if (t) {
    spinlock_acquire(&t->wait_entries_lock);
    if (entry->thread_prev)
      entry->thread_prev->thread_next = entry->thread_next;
    else if (t->wait_entries_head == entry)
      t->wait_entries_head = entry->thread_next;

    if (entry->thread_next)
      entry->thread_next->thread_prev = entry->thread_prev;

    entry->thread_prev = NULL;
    entry->thread_next = NULL;
    spinlock_release(&t->wait_entries_lock);
  }
}

void wait_queue_cleanup_thread(struct thread *t) {
  if (!t)
    return;

  spinlock_acquire(&t->wait_entries_lock);
  wait_queue_entry_t *curr = t->wait_entries_head;
  t->wait_entries_head = NULL;
  spinlock_release(&t->wait_entries_lock);

  while (curr) {
    wait_queue_entry_t *next = curr->thread_next;
    curr->thread_prev = NULL;
    curr->thread_next = NULL;
    wait_queue_t *wq = curr->wq;
    if (wq) {
      spinlock_acquire(&wq->lock);
      if (wq->head == curr) {
        wq->head = curr->next;
      } else {
        wait_queue_entry_t *p = wq->head;
        while (p && p->next != curr)
          p = p->next;
        if (p)
          p->next = curr->next;
      }
      curr->next = NULL;
      curr->thread = NULL;
      curr->wq = NULL;
      spinlock_release(&wq->lock);
    }
    curr = next;
  }
}

void wait_queue_wake_all(wait_queue_t *wq) {
  if (!wq)
    return;

  hal_irq_state_t rflags = hal_irq_save();
  spinlock_acquire(&wq->lock);

  wait_queue_entry_t *curr = wq->head;
  while (curr) {
    struct thread *t = curr->thread;
    if (t && t->state != THREAD_DEAD && t->state != THREAD_ZOMBIE) {
      sched_wakeup(t);
      t->wakeup_ticks = 0; // Clear any pending timeout
    }
    curr = curr->next;
  }

  spinlock_release(&wq->lock);
  hal_irq_restore(rflags);
}

void wait_queue_wake_one(wait_queue_t *wq) {
  if (!wq)
    return;

  hal_irq_state_t rflags = hal_irq_save();
  spinlock_acquire(&wq->lock);

  wait_queue_entry_t *curr = wq->head;
  while (curr) {
    struct thread *t = curr->thread;
    if (t && t->state != THREAD_DEAD && t->state != THREAD_ZOMBIE) {
      sched_wakeup(t);
      t->wakeup_ticks = 0;
      spinlock_release(&wq->lock);
      hal_irq_restore(rflags);
      return; // Only wake one thread
    }
    curr = curr->next;
  }

  spinlock_release(&wq->lock);
  hal_irq_restore(rflags);
}
