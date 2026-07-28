// Epoll Implementation

// Implements the epoll API for event multiplexing

#include "epoll.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "socket.h"
#include <stdint.h>

// Global Epoll Instance Table
static eventpoll_t *epoll_table[EPOLL_MAX_INSTANCES];
static int epoll_count = 0;
static spinlock_t epoll_table_lock = SPINLOCK_INIT;

/*
 * Notifications are normally reliable. Keep a short timeout on otherwise
 * unbounded waits as a safety net against a future producer missing a wakeup.
 * Timer ticks are expressed in milliseconds throughout the scheduler API.
 */
#define EPOLL_RESCAN_INTERVAL_MS 100

// Epoll Instance Table Management

static int epoll_table_alloc(void) {
  spinlock_acquire(&epoll_table_lock);

  for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
    if (epoll_table[i] == NULL) {
      epoll_count++;
      spinlock_release(&epoll_table_lock);
      return i;
    }
  }

  spinlock_release(&epoll_table_lock);
  return -1; // Table full
}

static void epoll_table_free(int idx) {
  if (idx < 0 || idx >= EPOLL_MAX_INSTANCES)
    return;

  spinlock_acquire(&epoll_table_lock);
  epoll_table[idx] = NULL;
  epoll_count--;
  spinlock_release(&epoll_table_lock);
}

// Epoll Item Management

static epitem_t *epitem_alloc(void) {
  epitem_t *epi = kmalloc(sizeof(epitem_t));
  if (!epi)
    return NULL;

  memset(epi, 0, sizeof(epitem_t));
  INIT_LIST_HEAD(&epi->rdllink);
  INIT_LIST_HEAD(&epi->fllink);
  INIT_LIST_HEAD(&epi->ep_node_link);
  spinlock_init(&epi->lock);
  return epi;
}

static void epitem_free(epitem_t *epi) {
  if (!epi)
    return;
  kfree(epi);
}

// Epoll Instance Creation/Destruction

eventpoll_t *epoll_create(void) {
  // Allocate epoll structure
  eventpoll_t *ep = kmalloc(sizeof(eventpoll_t));
  if (!ep) {
    klog_puts("[ERR] epoll: failed to allocate eventpoll structure\n");
    return NULL;
  }

  memset(ep, 0, sizeof(eventpoll_t));

  // Allocate table slot
  int idx = epoll_table_alloc();
  if (idx < 0) {
    kfree(ep);
    klog_puts("[ERR] epoll: epoll table full\n");
    return NULL;
  }

  // Initialize
  ep->fd = -1;
  ep->vfs_node = NULL;
  ep->item_count = 0;
  INIT_LIST_HEAD(&ep->rdllist);
  ep->rdllist_count = 0;
  wait_queue_init(&ep->wq);
  spinlock_init(&ep->lock);
  ep->refcount = 1;

  epoll_table[idx] = ep;

  return ep;
}

void epoll_destroy(eventpoll_t *ep) {
  if (!ep) {
    klog_puts("[WARN] epoll_destroy: NULL instance\n");
    return;
  }

  // Free all watched items and release the VFS references acquired by
  // EPOLL_CTL_ADD. Holding these references keeps watcher links valid even
  // when userspace closes a watched fd before closing the epoll fd.
  for (int i = 0; i < EPOLL_MAX_WATCHED; i++) {
    epitem_t *epi = ep->items[i];
    if (epi) {
      if (epi->on_ready_list) {
        list_del(&epi->rdllink);
        epi->on_ready_list = false;
      }
      if (epi->node) {
        spinlock_acquire(&epi->node->ep_lock);
        list_del(&epi->ep_node_link);
        spinlock_release(&epi->node->ep_lock);
        vfs_close(epi->node);
      }
      epitem_free(epi);
      ep->items[i] = NULL;
    }
  }

  // Remove from table
  for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
    if (epoll_table[i] == ep) {
      epoll_table_free(i);
      break;
    }
  }

  kfree(ep);
}

void epoll_get(eventpoll_t *ep) {
  if (!ep)
    return;
  __atomic_fetch_add(&ep->refcount, 1, __ATOMIC_ACQ_REL);
}

void epoll_put(eventpoll_t *ep) {
  if (!ep)
    return;

  if (__atomic_fetch_sub(&ep->refcount, 1, __ATOMIC_ACQ_REL) == 1) {
    epoll_destroy(ep);
  }
}

// Helper: Check if FD has events

static uint32_t ep_check_events(epitem_t *epi) {
  if (!epi || !epi->node)
    return 0;

  // Get the registered events, excluding modifier flags
  uint32_t watch_mask =
      epi->registered_events &
      ~(EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE | EPOLLWAKEUP);

  // Call VFS poll to get current events
  int revents = vfs_poll(epi->node, watch_mask);

  // Mask to only requested events plus error/hangup
  return (uint32_t)revents & (watch_mask | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
}

// Helper: Add item to ready list

static void ep_add_to_ready_list(eventpoll_t *ep, epitem_t *epi) {
  if (epi->on_ready_list)
    return;

  spinlock_acquire(&ep->lock);

  bool was_empty = (ep->rdllist_count == 0);
  if (!epi->on_ready_list) {
    list_add_tail(&epi->rdllink, &ep->rdllist);
    epi->on_ready_list = true;
    ep->rdllist_count++;
  }

  // Wake up waiters while still holding ep->lock.
  // This closes the race: epoll_wait_impl sets THREAD_BLOCKED while holding
  // ep->lock, so if we wake here (also under ep->lock) we are guaranteed to
  // see the correct blocked state and the thread won't miss the wakeup.
  if (epi->exclusive) {
    wait_queue_wake_one(&ep->wq);
  } else {
    wait_queue_wake_all(&ep->wq);
  }

  spinlock_release(&ep->lock);

  // Propagate to any outer epoll instances watching this epoll fd.
  // This is required for nested epoll (epoll-in-epoll) to work: when this
  // epoll's ready list becomes non-empty, any outer epoll that has registered
  // this epoll's VFS node must be woken up.
  if (was_empty && ep->vfs_node) {
    epoll_notify_event(ep->vfs_node, EPOLLIN);
  }
}

// Helper: Remove item from ready list

static void ep_remove_from_ready_list(eventpoll_t *ep, epitem_t *epi) {
  if (!epi->on_ready_list)
    return;

  spinlock_acquire(&ep->lock);

  if (epi->on_ready_list) {
    list_del(&epi->rdllink);
    epi->on_ready_list = false;
    ep->rdllist_count--;
  }

  spinlock_release(&ep->lock);
}

// epoll_ctl Operations

int epoll_ctl_add(eventpoll_t *ep, int fd, struct epoll_event *event) {
  if (!ep || !event)
    return -22; // EINVAL

  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF

  // Get current thread and VFS node for FD
  struct thread *t = sched_get_current();
  if (!t)
    return -1;

  if (fd >= MAX_FDS || !t->fds[fd]) {
    return -9; // EBADF
  }

  vfs_node_t *node = t->fds[fd];

  // Check if already registered for the same node
  if (ep->items[fd]) {
    epitem_t *old = ep->items[fd];
    if (old->node == node) {
      return -17; // EEXIST - same socket already registered
    }
    // Different node - fd was reused, remove stale item
    ep_remove_from_ready_list(ep, old);
    if (old->node) {
      spinlock_acquire(&old->node->ep_lock);
      list_del(&old->ep_node_link);
      spinlock_release(&old->node->ep_lock);
    }
    spinlock_acquire(&ep->lock);
    ep->items[fd] = NULL;
    ep->item_count--;
    spinlock_release(&ep->lock);
    vfs_close(old->node);
    epitem_free(old);
  }

  // Create epitem
  epitem_t *epi = epitem_alloc();
  if (!epi) {
    return -12; // ENOMEM
  }

  epi->fd = fd;
  epi->node = node;
  epi->event = *event;
  epi->ep = ep;
  epi->last_events = 0;
  epi->on_ready_list = false;
  epi->oneshot = (event->events & EPOLLONESHOT) != 0;
  epi->oneshot_disabled = false;
  epi->exclusive = (event->events & EPOLLEXCLUSIVE) != 0;
  epi->registered_events = event->events;

  // Keep the watched object alive until DEL or epoll destruction.
  vfs_open(node);

  // Add to epoll instance
  spinlock_acquire(&ep->lock);
  ep->items[fd] = epi;
  ep->item_count++;
  spinlock_release(&ep->lock);

  // Fast notification link
  spinlock_acquire(&node->ep_lock);
  if (node->ep_watchers.next == NULL) {
    INIT_LIST_HEAD(&node->ep_watchers);
  }
  list_add_tail(&epi->ep_node_link, &node->ep_watchers);
  spinlock_release(&node->ep_lock);

  // Check for immediate events. For level-triggered mode this is the normal
  // path. For edge-triggered mode, Linux also fires an initial synthetic edge
  // on EPOLL_CTL_ADD if the fd already has data — without this, any data
  // queued before the fd was added to epoll would be silently lost.
  uint32_t cur_events = ep_check_events(epi);
  if (cur_events) {
    epi->last_events = cur_events;
    ep_add_to_ready_list(ep, epi);
  }

  return 0;
}

int epoll_ctl_del(eventpoll_t *ep, int fd) {
  if (!ep)
    return -22; // EINVAL

  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF

  epitem_t *epi = ep->items[fd];
  if (!epi)
    return -2; // ENOENT

  // Remove from ready list if present
  ep_remove_from_ready_list(ep, epi);

  // Remove from epoll instance
  spinlock_acquire(&ep->lock);
  ep->items[fd] = NULL;
  ep->item_count--;
  spinlock_release(&ep->lock);

  // Unlink from node
  if (epi->node) {
    spinlock_acquire(&epi->node->ep_lock);
    list_del(&epi->ep_node_link);
    spinlock_release(&epi->node->ep_lock);
  }

  // Release the watched object reference, then free the item.
  vfs_close(epi->node);
  epitem_free(epi);

  return 0;
}

int epoll_ctl_mod(eventpoll_t *ep, int fd, struct epoll_event *event) {
  if (!ep || !event)
    return -22; // EINVAL

  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF

  epitem_t *epi = ep->items[fd];
  if (!epi)
    return -2; // ENOENT

  // Update event mask
  spinlock_acquire(&epi->lock);
  epi->event = *event;
  epi->oneshot = (event->events & EPOLLONESHOT) != 0;
  epi->exclusive = (event->events & EPOLLEXCLUSIVE) != 0;
  epi->registered_events = event->events; // Always update registered_events

  // Reset oneshot disabled state when re-arming via EPOLL_CTL_MOD
  if (epi->oneshot) {
    epi->oneshot_disabled = false;
  }
  spinlock_release(&epi->lock);

  // Remove from ready list and re-check
  ep_remove_from_ready_list(ep, epi);

  // Re-check events
  uint32_t events = ep_check_events(epi);
  if (events) {
    epi->last_events = events;
    ep_add_to_ready_list(ep, epi);
  }

  return 0;
}

// epoll_wait Implementation

int epoll_wait_impl(eventpoll_t *ep, struct epoll_event *events, int maxevents,
                    int timeout_ms) {
  if (!ep || !events || maxevents <= 0)
    return -22; // EINVAL

  int returned = 0;
  struct thread *current = sched_get_current();

  // Add to wait queue once for the duration of the wait
  wait_queue_entry_t entry;
  entry.thread = current;
  entry.next = NULL;
  wait_queue_add(&ep->wq, &entry);

  while (returned == 0) {
    spinlock_acquire(&ep->lock);

    // If ready list has items, return them immediately
    if (!list_empty(&ep->rdllist)) {
      struct list_head *pos, *n;
      list_for_each_safe(pos, n, &ep->rdllist) {
        if (returned >= maxevents)
          break;

        epitem_t *epi = list_entry(pos, epitem_t, rdllink);

        // Get current events - this may call VFS poll which could take other
        // locks Normally we should be careful about lock ordering, but ep->lock
        // is likely safe as it's a leaf structure's lock.
        uint32_t current_events = ep_check_events(epi);

        if (current_events) {
          events[returned].events = current_events;
          events[returned].data.u64 = epi->event.data.u64;

          returned++;

          // Handle edge-triggered mode
          if (epi->event.events & EPOLLET) {
            list_del(&epi->rdllink);
            epi->on_ready_list = false;
            ep->rdllist_count--;
            // Track events for next edge
            epi->last_events = current_events &
                               (EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM);
          }

          // Handle oneshot mode
          if (epi->oneshot) {
            epi->oneshot_disabled = true;
            epi->registered_events = 0;
            if (!(epi->event.events & EPOLLET)) {
              list_del(&epi->rdllink);
              epi->on_ready_list = false;
              ep->rdllist_count--;
            }
          }
        } else {
          // No longer has events, remove from ready list
          list_del(&epi->rdllink);
          epi->on_ready_list = false;
          ep->rdllist_count--;
        }
      }

      if (returned > 0) {
        spinlock_release(&ep->lock);
        break;
      }
    }

    // No events ready - handle immediate timeout
    if (timeout_ms == 0) {
      spinlock_release(&ep->lock);
      break;
    }

    // Set state to BLOCKED while still holding ep->lock.
    // This closes the race window: if a notification arrives now, it will
    // call ep_add_to_ready_list (which acquires ep->lock and will block
    // until we release it below), then call wait_queue_wake_all which will
    // see THREAD_BLOCKED and properly call sched_wakeup().
    current->state = THREAD_BLOCKED;

    // Set up timeout if specified.
    // Save the absolute deadline in a local variable — the scheduler clears
    // wakeup_ticks to 0 when it wakes the thread, so we cannot rely on it
    // after sched_yield() returns.
    uint64_t deadline_ticks = 0;
    if (timeout_ms > 0 && timeout_ms != -1) {
      deadline_ticks = lapic_timer_get_ticks() + (uint64_t)timeout_ms;
      current->wakeup_ticks = deadline_ticks;
    } else if (timeout_ms == -1) {
      /*
       * An infinite epoll wait must still recover if a producer ever fails
       * to notify us. This is not a user-visible timeout: after waking we
       * re-poll watched descriptors and continue waiting if none are ready.
       */
      current->wakeup_ticks =
          lapic_timer_get_ticks() + EPOLL_RESCAN_INTERVAL_MS;
    } else {
      current->wakeup_ticks = 0;
    }

    // Re-check the ready list one more time before yielding.
    // A notification may have added items to the ready list between our
    // first check and setting THREAD_BLOCKED above.  If so, cancel the
    // block and loop back to collect the events.
    if (!list_empty(&ep->rdllist)) {
      current->state = THREAD_RUNNING;
      current->wakeup_ticks = 0;
      spinlock_release(&ep->lock);
      continue;
    }

    spinlock_release(&ep->lock);

    // Yield control
    sched_yield();

    // After waking up, reset state to RUNNING.
    // NOTE: Do this BEFORE re-acquiring ep->lock so the lock acquisition
    // itself doesn't race with another notification.
    current->state = THREAD_RUNNING;

    // After waking up, check if it was due to a timeout
    if (timeout_ms > 0 && timeout_ms != -1) {
      if (deadline_ticks != 0 && lapic_timer_get_ticks() >= deadline_ticks) {
        current->wakeup_ticks = 0;
        break; // Return whatever we found (likely 0)
      }
    }

    // Missed-wakeup recovery: scan all watched items for events.
    // Race: a notification may fire between sched_yield() returning and
    // THREAD_BLOCKED being set in the next loop iteration.  sched_wakeup()
    // sees THREAD_RUNNING and skips the wakeup, leaving data in the queue
    // with no new notification.  Re-polling every watched item here catches
    // that case.  ep_check_events calls vfs_poll which may acquire other
    // locks, so we poll outside ep->lock, then add under ep->lock.
    for (int _i = 0; _i < EPOLL_MAX_WATCHED; _i++) {
      epitem_t *_epi = ep->items[_i];
      if (!_epi || _epi->on_ready_list)
        continue;
      if (_epi->oneshot && _epi->oneshot_disabled)
        continue;
      uint32_t _ev = ep_check_events(_epi);
      if (_ev) {
        spinlock_acquire(&ep->lock);
        if (!_epi->on_ready_list) {
          _epi->last_events = _ev;
          list_add_tail(&_epi->rdllink, &ep->rdllist);
          _epi->on_ready_list = true;
          ep->rdllist_count++;
        }
        spinlock_release(&ep->lock);
      }
    }

    // Loop back and collect any events found above or via normal notification.
  }

  // Always remove from wait queue before returning
  wait_queue_remove(&ep->wq, &entry);
  current->state = THREAD_RUNNING;
  current->wakeup_ticks = 0;

  return returned;
}

// Epoll FD Management

int epoll_alloc_fd(eventpoll_t *ep) {
  struct thread *t = sched_get_current();
  if (!t)
    return -1;

  int fd = alloc_fd(t);
  if (fd < 0)
    return -24; // EMFILE

  // Create VFS node for epoll instance
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node) {
    return -12; // ENOMEM
  }

  vfs_node_init(node);
  node->flags = FS_EPOLL;
  node->inode = (uint32_t)(uint64_t)ep;
  node->device = ep;
  node->wait_queue = &ep->wq;

  // Set epoll VFS operations
  node->read = epoll_vfs_read;
  node->write = epoll_vfs_write;
  node->open = epoll_vfs_open;
  node->close = epoll_vfs_close;
  node->poll = epoll_vfs_poll;

  ep->fd = fd;
  ep->vfs_node = node; // Store for nested epoll propagation
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;

  // vfs_node_init() created the node with refcount 1; that initial node
  // reference is the fd-table ownership. The epoll object's initial
  // reference is transferred to the node and released by epoll_vfs_close().

  return fd;
}

eventpoll_t *epoll_from_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t)
    return NULL;

  if (fd < 0 || fd >= MAX_FDS)
    return NULL;

  vfs_node_t *node = t->fds[fd];
  if (!node)
    return NULL;

  if ((node->flags & FS_TYPE_MASK) != FS_EPOLL)
    return NULL;

  return (eventpoll_t *)node->device;
}

int epoll_close_fd(int fd) {
  struct thread *t = sched_get_current();
  if (!t)
    return -9; // EBADF

  if (fd < 0 || fd >= MAX_FDS)
    return -9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if (!node)
    return -9; // EBADF

  if ((node->flags & FS_TYPE_MASK) != FS_EPOLL)
    return -22; // EINVAL

  eventpoll_t *ep = (eventpoll_t *)node->device;

  // Clear FD
  t->fds[fd] = NULL;
  t->fd_offsets[fd] = 0;

  // Release epoll reference through VFS close
  if (node) {
    vfs_close(node);
  }

  // Release the initial reference from epoll_create()
  if (ep) {
    epoll_put(ep);
  }

  return 0;
}

// Epoll VFS Operations

uint32_t epoll_vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  // epoll instances are not readable via read()
  return 0;
}

uint32_t epoll_vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size,
                         uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  // epoll instances are not writable via write()
  return 0;
}

void epoll_vfs_open(struct vfs_node *node) {
  if (!node)
    return;
  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (ep)
    epoll_get(ep);
}

void epoll_vfs_close(struct vfs_node *node) {
  if (!node)
    return;
  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (ep)
    epoll_put(ep);
}

int epoll_vfs_poll(struct vfs_node *node, int events) {
  if (!node)
    return POLLNVAL;

  eventpoll_t *ep = (eventpoll_t *)node->device;
  if (!ep)
    return POLLNVAL;

  int revents = 0;

  // Check if ready list has items
  spinlock_acquire(&ep->lock);
  if (!list_empty(&ep->rdllist)) {
    if (events & POLLIN)
      revents |= POLLIN;
  }
  // Always writable (for poll purposes)
  if (events & POLLOUT)
    revents |= POLLOUT;
  spinlock_release(&ep->lock);

  return revents;
}

// Event Notification

void epoll_notify_event(struct vfs_node *node, uint32_t events) {
  if (!node)
    return;

  // Lazy-init check for node notification list
  if (node->ep_watchers.next == NULL)
    return;

  spinlock_acquire(&node->ep_lock);
  struct list_head *pos, *n;
  list_for_each_safe(pos, n, &node->ep_watchers) {
    epitem_t *epi = list_entry(pos, epitem_t, ep_node_link);
    eventpoll_t *ep = epi->ep;

    // Skip disabled oneshot items
    if (epi->oneshot && epi->oneshot_disabled)
      continue;

    uint32_t mask =
        events & (epi->registered_events | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
    if (!mask)
      continue;

    // ep_add_to_ready_list now handles both adding to the ready list AND
    // waking up waiters atomically under ep->lock, eliminating the
    // missed-wakeup race.
    ep_add_to_ready_list(ep, epi);

    // EPOLLEXCLUSIVE: stop after waking the first matching watcher so only
    // one epoll instance is woken per event (prevents thundering herd).
    if (epi->exclusive)
      break;
  }
  spinlock_release(&node->ep_lock);
}

// Notify epoll by socket FD (for abstract sockets without VFS node)
void epoll_notify_socket(int fd, uint32_t events) {
  if (fd < 0)
    return;

  // Find all epoll instances watching this FD
  for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
    eventpoll_t *ep = epoll_table[i];
    if (!ep)
      continue;

    // Find epitem for this FD
    epitem_t *epi = ep->items[fd];
    if (!epi)
      continue;

    // Skip disabled oneshot items
    if (epi->oneshot && epi->oneshot_disabled)
      continue;

    uint32_t mask =
        events & (epi->registered_events | EPOLLERR | EPOLLHUP | EPOLLRDHUP);

    if (mask) {
      // ep_add_to_ready_list handles both adding to the ready list AND
      // waking up waiters atomically under ep->lock.
      ep_add_to_ready_list(ep, epi);
    }
  }
}

// Epoll Subsystem Initialization

void epoll_init(void) {
  memset(epoll_table, 0, sizeof(epoll_table));
  epoll_count = 0;
  spinlock_init(&epoll_table_lock);

  klog_puts("[OK] Epoll subsystem initialized (max instances: ");
  klog_uint64(EPOLL_MAX_INSTANCES);
  klog_puts(", max watched FDs: ");
  klog_uint64(EPOLL_MAX_WATCHED);
  klog_puts(")\n");
}
