// ── Epoll Implementation
// ─────────────────────────────────────── Phase 8: epoll Infrastructure
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

// ── Global Epoll Instance Table
// ───────────────────────────────────────────────────────
static eventpoll_t *epoll_table[EPOLL_MAX_INSTANCES];
static int epoll_count = 0;
static spinlock_t epoll_table_lock = SPINLOCK_INIT;

// ── Epoll Instance Table Management
// ────────────────────────────────────────────────────────

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



// ── Epoll Item Management
// ───────────────────────────────────────────────────────

static epitem_t *epitem_alloc(void) {
  epitem_t *epi = kmalloc(sizeof(epitem_t));
  if (!epi)
    return NULL;

  memset(epi, 0, sizeof(epitem_t));
  INIT_LIST_HEAD(&epi->rdllink);
  INIT_LIST_HEAD(&epi->fllink);
  spinlock_init(&epi->lock);
  return epi;
}

static void epitem_free(epitem_t *epi) {
  if (!epi)
    return;
  kfree(epi);
}

// ── Epoll Instance Creation/Destruction
// ─────────────────────────────────────────────────────────

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
  ep->item_count = 0;
  INIT_LIST_HEAD(&ep->rdllist);
  ep->rdllist_count = 0;
  wait_queue_init(&ep->wq);
  spinlock_init(&ep->lock);
  ep->refcount = 1;

  epoll_table[idx] = ep;

  klog_puts("[EPOLL] Created epoll instance=");
  klog_uint64((uint64_t)ep);
  klog_puts("\n");

  epoll_get(ep);
  return ep;
}

void epoll_destroy(eventpoll_t *ep) {
  if (!ep) {
    klog_puts("[WARN] epoll_destroy: NULL instance\n");
    return;
  }

  spinlock_acquire(&ep->lock);

  // Free all watched items
  for (int i = 0; i < EPOLL_MAX_WATCHED; i++) {
    if (ep->items[i]) {
      epitem_free(ep->items[i]);
      ep->items[i] = NULL;
    }
  }

  spinlock_release(&ep->lock);

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

// ── Helper: Check if FD has events
// ────────────────────────────────────────────────────────

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

// ── Helper: Add item to ready list
// ────────────────────────────────────────────────────────

static void ep_add_to_ready_list(eventpoll_t *ep, epitem_t *epi) {
  if (epi->on_ready_list)
    return;

  spinlock_acquire(&ep->lock);

  if (!epi->on_ready_list) {
    list_add_tail(&epi->rdllink, &ep->rdllist);
    epi->on_ready_list = true;
    ep->rdllist_count++;
  }

  spinlock_release(&ep->lock);
}

// ── Helper: Remove item from ready list
// ─────────────────────────────────────────────────────────────

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

// ── epoll_ctl Operations
// ───────────────────────────────────────────────────────

int epoll_ctl_add(eventpoll_t *ep, int fd, struct epoll_event *event) {
  if (!ep || !event)
    return -22; // EINVAL

  klog_puts("[EPOLL_CTL_ADD] fd=");
  klog_uint64(fd);
  klog_puts(" epoll_fd=");
  klog_uint64(ep->fd);
  klog_puts("\n");

  if (fd < 0 || fd >= EPOLL_MAX_WATCHED)
    return -9; // EBADF

  // Get current thread and VFS node for FD
  struct thread *t = sched_get_current();
  if (!t)
    return -1;

  if (fd >= MAX_FDS || !t->fds[fd]) {
    klog_puts("[EPOLL_CTL_ADD] EBADF: fd=");
    klog_uint64(fd);
    klog_puts(" does not exist in process\n");
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
    spinlock_acquire(&ep->lock);
    ep->items[fd] = NULL;
    ep->item_count--;
    spinlock_release(&ep->lock);
    epitem_free(old);
  }

  // Create epitem
  epitem_t *epi = epitem_alloc();
  if (!epi) {
    klog_puts("[EPOLL_CTL_ADD] ENOMEM: failed to alloc epitem\n");
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

  // Check for immediate events (level-triggered)
  if (!(event->events & EPOLLET)) {
    uint32_t events = ep_check_events(epi);
    if (events) {
      epi->last_events = events;
      ep_add_to_ready_list(ep, epi);
    }
  }

  klog_puts("[EPOLL_CTL_ADD] finished fd=");
  klog_uint64(fd);
  klog_puts("\n");

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

  // Free epitem
  epitem_free(epi);

  klog_puts("[EPOLL_CTL_DEL] finished fd=");
  klog_uint64(fd);
  klog_puts("\n");

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

  klog_puts("[EPOLL_CTL_MOD] finished fd=");
  klog_uint64(fd);
  klog_puts("\n");

  return 0;
}

// ── epoll_wait Implementation
// ────────────────────────────────────────────────────────

int epoll_wait_impl(eventpoll_t *ep, struct epoll_event *events, int maxevents,
                    int timeout_ms) {
  if (!ep || !events || maxevents <= 0)
    return -22; // EINVAL

  int returned = 0;
  struct thread *current = sched_get_current();

  klog_puts("[EPOLL_WAIT] tid=");
  klog_uint64(current->tid);
  klog_puts(" epoll_fd=");
  klog_uint64(ep->fd);
  klog_puts(" timeout=");
  klog_uint64((uint64_t)timeout_ms);
  klog_puts("\n");

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

        // Get current events - this may call VFS poll which could take other locks
        // Normally we should be careful about lock ordering, but ep->lock is
        // likely safe as it's a leaf structure's lock.
        uint32_t current_events = ep_check_events(epi);

        if (current_events) {
          events[returned].events = current_events;
          events[returned].data.u64 = epi->event.data.u64;
          
          klog_puts("[EPOLL_EVENT] returned fd=");
          klog_uint64(epi->fd);
          klog_puts(" events=0x");
          klog_hex32(current_events);
          klog_puts("\n");
          
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

    // Set state to BLOCKED while holding the lock.
    // This ensures that any concurrent epoll_notify_event will see the thread as
    // ready-to-be-woken when it tries to take the same lock.
    current->state = THREAD_BLOCKED;

    // Set up timeout if specified
    if (timeout_ms > 0 && timeout_ms != -1) {
      current->wakeup_ticks = lapic_timer_get_ticks() + (uint64_t)timeout_ms;
    } else {
      current->wakeup_ticks = 0;
    }

    spinlock_release(&ep->lock);

    // Yield control
    sched_yield();

    // After waking up, check if it was due to a timeout
    if (timeout_ms > 0 && timeout_ms != -1) {
      if (lapic_timer_get_ticks() >= current->wakeup_ticks && current->wakeup_ticks != 0) {
        break; // Return whatever we found (likely 0)
      }
    }

    // If we were woken up, loop back and check the ready list again
  }

  // Always remove from wait queue before returning
  wait_queue_remove(&ep->wq, &entry);
  current->state = THREAD_RUNNING;
  current->wakeup_ticks = 0;

  klog_puts("[EPOLL_WAIT] returned count=");
  klog_uint64(returned);
  klog_puts("\n");

  return returned;
}

// ── Epoll FD Management
// ───────────────────────────────────────────────────────

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
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;

  vfs_open(node);

  // Release the initial creation reference, as the VFS node now owns it
  epoll_put(ep);

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

// ── Epoll VFS Operations
// ───────────────────────────────────────────────────────

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

// ── Event Notification
// ────────────────────────────────────────────────────────

void epoll_notify_event(struct vfs_node *node, uint32_t events) {
  if (!node)
    return;

  // Lazy-init check for node notification list
  if (node->ep_watchers.next == NULL)
    return;

  // Track if we've woken an exclusive waiter for THIS node.
  // EPOLLEXCLUSIVE prevents thundering herd: only one exclusive waiter
  // per event source should be woken. Non-exclusive waiters are unaffected.
  bool exclusive_woken = false;

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

    // Add to ready list
    ep_add_to_ready_list(ep, epi);

    // Wake up waiters
    if (epi->exclusive) {
      // EPOLLEXCLUSIVE: only wake one thread across all exclusive waiters
      // to prevent thundering herd. Skip if we already woke one.
      if (!exclusive_woken) {
        wait_queue_wake_one(&ep->wq);
        exclusive_woken = true;
      }
    } else {
      // Non-exclusive: always wake all threads waiting on this epoll
      wait_queue_wake_all(&ep->wq);
    }
  }
  spinlock_release(&node->ep_lock);
}

// Notify epoll by socket FD (for abstract sockets without VFS node)
void epoll_notify_socket(int fd, uint32_t events) {
  if (fd < 0)
    return;

  klog_puts("[EPOLL] notify_socket: fd=");
  klog_uint64(fd);
  klog_puts(" events=");
  klog_uint64(events);
  klog_puts("\n");

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
      // Add to ready list
      ep_add_to_ready_list(ep, epi);

      // Wake up waiters - EPOLLEXCLUSIVE only affects THIS epoll instance,
      // not others. Each epoll instance should be woken independently.
      if (epi->exclusive) {
        // For exclusive mode, only wake one thread waiting on this epoll
        wait_queue_wake_one(&ep->wq);
      } else {
        // Non-exclusive: wake all threads waiting on this epoll
        wait_queue_wake_all(&ep->wq);
      }
    }
  }
}

// ── Epoll Subsystem Initialization
// ───────────────────────────────────────────────────────

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
