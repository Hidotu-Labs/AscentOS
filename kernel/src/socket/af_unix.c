// ── AF_UNIX Socket Family Implementation
// ─────────────────────────────────────── Phase 2: Socket Families & Protocols

#include "af_unix.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/list.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "epoll.h"
#include "socket.h"
#include "socket_internal.h"
#include <stddef.h>
#include <stdint.h>

// ── AF_UNIX Bound Sockets Tracking ───────────────────────────────────────────

static struct list_head unix_bound_list;
static spinlock_t unix_bound_lock;

/**
 * Find an AF_UNIX socket bound to a specific address.
 */
unix_sock_t *unix_find_socket_by_addr(struct sockaddr_un *addr, int addrlen) {
  struct list_head *pos;

  // Lock list for searching
  spinlock_acquire(&unix_bound_lock);

  list_for_each(pos, &unix_bound_list) {
    unix_sock_t *usk = list_entry(pos, unix_sock_t, bind_node);

    // Check family
    if (usk->addr.sun_family != AF_UNIX)
      continue;

    // check sun_path
    if (usk->addr.sun_path[0] == '\0') {
      // Abstract socket: must match exact length and bytes
      if (usk->addr_len != addrlen)
        continue;
      if (memcmp(usk->addr.sun_path, addr->sun_path,
                 addrlen - offsetof(struct sockaddr_un, sun_path)) == 0) {
        spinlock_release(&unix_bound_lock);
        return usk;
      }
    } else {
      // Filesystem socket: just compare path strings
      if (strcmp(usk->addr.sun_path, addr->sun_path) == 0) {
        spinlock_release(&unix_bound_lock);
        return usk;
      }
    }
  }

  spinlock_release(&unix_bound_lock);
  return NULL;
}

/**
 * Mark a socket's filesystem entry as unlinked.
 * Called when a socket file is unlinked.
 * The socket remains in the bound list so re-binding fails with EADDRINUSE.
 * Returns 0 on success, -1 if not found.
 */
int unix_unbind_by_path(const char *path) {
  if (!path || path[0] == '\0')
    return -1;

  struct list_head *pos, *n;
  int found = 0;

  spinlock_acquire(&unix_bound_lock);

  list_for_each_safe(pos, n, &unix_bound_list) {
    unix_sock_t *usk = list_entry(pos, unix_sock_t, bind_node);

    // Skip abstract sockets
    if (usk->addr.sun_path[0] == '\0')
      continue;

    // Compare filesystem paths
    if (strcmp(usk->addr.sun_path, path) == 0) {
      // Keep in bound list - re-binding should fail with EADDRINUSE
      // Just clear the VFS node pointer since file is gone
      usk->parent->node = NULL;
      found = 1;
      break;
    }
  }

  spinlock_release(&unix_bound_lock);
  return found ? 0 : -1;
}

// ── AF_UNIX Family Structure
// ───────────────────────────────────────────────────

static net_family_t unix_family = {
    .family = AF_UNIX, .create = unix_create, .next = NULL};

// ── Local structure definitions for socket options
// ───────────────────────────── These are used for SO_PEERCRED and
// SO_RCVTIMEO/SO_SNDTIMEO
struct ucred_local {
  int pid;
  int uid;
  int gid;
};

struct timeval_local {
  int64_t tv_sec;
  int64_t tv_usec;
};

#define UCRED_SIZE sizeof(struct ucred_local)
#define TIMEVAL_SIZE sizeof(struct timeval_local)

// ── AF_UNIX Socket Operations (stubs for Phase 2)
// ────────────────────────────── Full implementations will be in later phases

static int unix_bind_abstract(unix_sock_t *usk, struct sockaddr_un *sun,
                              int addrlen) {
  // Check if address is already in use
  if (unix_find_socket_by_addr(sun, addrlen)) {
    return -EADDRINUSE;
  }

  // Copy address
  memcpy(&usk->addr, sun, addrlen);
  usk->addr_len = addrlen;
  usk->is_abstract = true;

  // Add to bound list
  spinlock_acquire(&unix_bound_lock);
  list_add(&usk->bind_node, &unix_bound_list);
  spinlock_release(&unix_bound_lock);

  usk->parent->state = SS_UNCONNECTED;

  klog_puts("[OK] unix_bind: bound to abstract address\n");
  return 0;
}

static int unix_bind_fs(unix_sock_t *usk, struct sockaddr_un *sun,
                        int addrlen) {
  // Filesystem-bound socket
  // sun_path is a null-terminated string

  // Check if already bound (internal list)
  if (unix_find_socket_by_addr(sun, addrlen)) {
    klog_puts("[WARN] unix_bind_fs: address already in internal bound list\n");
    return -EADDRINUSE;
  }

  // Need to create the node. Find parent directory.
  char parent_path[UNIX_PATH_MAX];
  char name[UNIX_PATH_MAX];

  // Very basic path splitting (doesn't handle all edge cases but sufficient for
  // Phase 3)
  const char *last_slash = strrchr(sun->sun_path, '/');
  if (!last_slash) {
    // Current directory
    strcpy(parent_path, ".");
    strcpy(name, sun->sun_path);
  } else {
    int dir_len = last_slash - sun->sun_path;
    if (dir_len == 0) {
      strcpy(parent_path, "/");
    } else {
      memcpy(parent_path, sun->sun_path, dir_len);
      parent_path[dir_len] = '\0';
    }
    strcpy(name, last_slash + 1);
  }

  struct thread *current_thread = sched_get_current();
  vfs_node_t *cwd_node = fs_root;
  if (current_thread && current_thread->cwd_path[0]) {
    cwd_node = vfs_resolve_path(current_thread->cwd_path);
    if (!cwd_node)
      cwd_node = fs_root;
  }

  // Check if target file already exists
  vfs_node_t *existing = vfs_resolve_path_at(cwd_node, sun->sun_path);
  if (existing) {
    // Already exists
    klog_puts("[WARN] unix_bind_fs: filesystem node already exists: ");
    klog_puts(sun->sun_path);
    klog_puts("\n");
    return -EADDRINUSE;
  }

  vfs_node_t *parent = vfs_resolve_path_at(cwd_node, parent_path);
  if (!parent) {
    klog_puts("[WARN] unix_bind_fs: parent directory not found\n");
    return -2; // ENOENT
  }

  // Create socket node
  // flags = FS_SOCKET
  int ret = vfs_mknod(parent, name, 0777, FS_SOCKET, usk->parent);
  if (ret < 0) {
    return ret;
  }

  // Success. Get the new node.
  usk->parent->node = vfs_finddir(parent, name);

  // Copy address
  memcpy(&usk->addr, sun, addrlen);
  usk->addr_len = addrlen;
  usk->is_abstract = false;

  // Add to bound list
  spinlock_acquire(&unix_bound_lock);
  list_add(&usk->bind_node, &unix_bound_list);
  spinlock_release(&unix_bound_lock);

  usk->parent->state = SS_UNCONNECTED;

  klog_puts("[OK] unix_bind: bound to filesystem path: ");
  klog_puts(sun->sun_path);
  klog_puts("\n");

  return 0;
}

static int unix_bind(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (!sock || !addr)
    return -22; // EINVAL
  if (addr->sa_family != AF_UNIX)
    return -97; // EAFNOSUPPORT
  if ((size_t)addrlen < offsetof(struct sockaddr_un, sun_path))
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;

  // Ensure not already bound
  if (usk->addr_len > 0)
    return -22; // EINVAL (already bound)

  struct sockaddr_un *sun = (struct sockaddr_un *)addr;

  if (sun->sun_path[0] == '\0') {
    // Abstract namespace
    return unix_bind_abstract(usk, sun, addrlen);
  } else {
    // Filesystem namespace
    return unix_bind_fs(usk, sun, addrlen);
  }
}

static int unix_listen(socket_t *sock, int backlog) {
  if (!sock)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;

  spinlock_acquire(&sock->lock);

  usk->is_listener = true;
  usk->backlog = (backlog > 0) ? backlog : 128;
  usk->accept_queue_len = 0;
  usk->accept_next = NULL;
  sock->state = SS_LISTENING;

  spinlock_release(&sock->lock);

  klog_puts("[OK] unix_listen: socket is now listening (backlog=");
  klog_uint64(usk->backlog);
  klog_puts(")\n");

  return 0;
}

static int unix_connect(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (!sock || !addr)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;

  if (sock->state == SS_CONNECTED)
    return -106; // EISCONN
  if (sock->state == SS_LISTENING)
    return -22; // EINVAL

  struct sockaddr_un *sun = (struct sockaddr_un *)addr;

  // Find the listener socket by address
  unix_sock_t *dusk = unix_find_socket_by_addr(sun, addrlen);
  if (!dusk) {
    klog_puts("[WARN] unix_connect: destination not found\n");
    return -111; // ECONNREFUSED
  }

  klog_puts("[UNIX_CONNECT] found listener fd=");
  klog_uint64(dusk->parent->fd);
  klog_puts(" is_abstract=");
  klog_uint64(dusk->is_abstract ? 1 : 0);
  klog_puts(" has_node=");
  klog_uint64(dusk->parent->node ? 1 : 0);
  klog_puts("\n");

  if (dusk->parent->state != SS_LISTENING) {
    klog_puts("[WARN] unix_connect: destination is not listening\n");
    return -111; // ECONNREFUSED
  }

  // Create the server-side socket for this connection EARLY
  socket_t *server_sock =
      socket_create(sock->domain, sock->type, sock->protocol);
  if (!server_sock) {
    return -12; // ENOMEM
  }
  unix_sock_t *server_usk = (unix_sock_t *)server_sock->sk;

  // Establish the peer relationship IMMEDIATELY
  usk->peer = server_usk;
  server_usk->peer = usk;

  // Update states
  sock->state = SS_CONNECTED;
  server_sock->state = SS_CONNECTED;

  // Lock the listener to add the server socket to its accept queue
  spinlock_acquire(&dusk->parent->lock);

  if (dusk->accept_queue_len >= dusk->backlog) {
    spinlock_release(&dusk->parent->lock);
    klog_puts("[WARN] unix_connect: listener backlog full\n");
    usk->peer = NULL;
    server_usk->peer = NULL;
    socket_put(server_sock);
    return -111; // ECONNREFUSED
  }

  // Add the server socket to the accept queue
  server_usk->accept_next = NULL;
  if (dusk->accept_next == NULL) {
    dusk->accept_next = server_usk;
  } else {
    unix_sock_t *curr = dusk->accept_next;
    while (curr->accept_next) {
      curr = curr->accept_next;
    }
    curr->accept_next = server_usk;
  }
  dusk->accept_queue_len++;

  // Notify listener
  if (dusk->is_abstract) {
    epoll_notify_socket(dusk->parent->fd, POLLIN);
  } else if (dusk->parent->node) {
    epoll_notify_event(dusk->parent->node, POLLIN);
  }
  wait_queue_wake_all(dusk->wait);

  spinlock_release(&dusk->parent->lock);

  return 0;
}

static int unix_accept(socket_t *sock, socket_t **newsock) {
  if (!sock || !newsock)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk || !usk->is_listener)
    return -22; // EINVAL

  spinlock_acquire(&sock->lock);

  while (usk->accept_next == NULL) {
    if (sock->flags & SOCK_NONBLOCK) {
      spinlock_release(&sock->lock);
      return -11; // EAGAIN
    }

    struct thread *current = sched_get_current();
    wait_queue_entry_t entry;
    entry.thread = current;
    entry.next = NULL;

    wait_queue_add(usk->wait, &entry);
    current->state = THREAD_BLOCKED;

    // Check again while BLOCKED
    if (usk->accept_next != NULL) {
      current->state = THREAD_RUNNING;
      spinlock_release(&sock->lock);
    } else {
      spinlock_release(&sock->lock);
      sched_yield();
      spinlock_acquire(&sock->lock);
    }

    wait_queue_remove(usk->wait, &entry);
    current->state = THREAD_RUNNING;
  }

  // Dequeue the already-created server socket
  unix_sock_t *new_usk = usk->accept_next;
  usk->accept_next = new_usk->accept_next;
  usk->accept_queue_len--;

  spinlock_release(&sock->lock);

  socket_t *new_sock = new_usk->parent;
  new_usk->is_accepted = true;

  *newsock = new_sock;

  klog_puts("[OK] unix_accept: retrieved early-linked connection\n");
  return 0;
}

static ssize_t unix_send(socket_t *sock, const void *buf, size_t len,
                         int flags) {
  (void)flags;
  if (!sock || !sock->sk)
    return -9; // EBADF

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  if (sock->state != SS_CONNECTED && sock->state != SS_CONNECTING) {
    return -107; // ENOTCONN
  }

  // If in CONNECTING state, block until peer is set (accept() called)
  while (usk->peer == NULL && sock->state == SS_CONNECTING) {
    // Check if listener was closed or we were orphaned
    if (usk->listener == NULL || usk->orphaned) {
      return -107; // ENOTCONN
    }

    // Block until accept() sets peer
    struct thread *current = sched_get_current();
    wait_queue_entry_t entry;
    entry.thread = current;
    entry.next = NULL;

    wait_queue_add(usk->wait, &entry);
    current->state = THREAD_BLOCKED;

    // Re-check peer/state
    if (usk->peer != NULL || sock->state != SS_CONNECTING) {
      current->state = THREAD_RUNNING;
    } else {
      sched_yield();
    }

    wait_queue_remove(usk->wait, &entry);
    current->state = THREAD_RUNNING;
  }

  unix_sock_t *peer = usk->peer;
  if (!peer)
    return -107; // ENOTCONN

  size_t sent = 0;
  const uint8_t *src = (const uint8_t *)buf;

  while (sent < len) {
    spinlock_acquire(&peer->recv_lock);

    // Calculate space in peer's receive buffer
    size_t head = peer->recv_buf_head;
    size_t tail = peer->recv_buf_tail;
    size_t size = peer->recv_buf_size;
    size_t space = (head - tail - 1 + size) % size;

    if (space == 0) {
      spinlock_release(&peer->recv_lock);

      if (sent > 0)
        break; // Return what we sent so far

      if ((sock->flags & SOCK_NONBLOCK) ||
          (flags & 0x40)) { // MSG_DONTWAIT is 0x40
        return -11;         // EAGAIN
      }

      // Block until space available
      struct thread *current = sched_get_current();
      wait_queue_entry_t entry;
      entry.thread = current;
      entry.next = NULL;

      wait_queue_add(peer->wait, &entry);
      current->state = THREAD_BLOCKED;

      // Re-acquire peer lock (actually we already have it at this point?
      // Line 511 acquired it).
      // Re-cache space
      size = peer->recv_buf_size;
      space = (peer->recv_buf_head - peer->recv_buf_tail - 1 + size) % size;

      if (space > 0) {
        current->state = THREAD_RUNNING;
        spinlock_release(&peer->recv_lock);
      } else {
        spinlock_release(&peer->recv_lock);
        sched_yield();
        spinlock_acquire(&peer->recv_lock);
      }

      wait_queue_remove(peer->wait, &entry);
      current->state = THREAD_RUNNING;

      if (sock->error)
        return -sock->error;
      continue;
    }

    size_t to_copy = (len - sent < space) ? len - sent : space;

    // Copy to ring buffer
    for (size_t i = 0; i < to_copy; i++) {
      peer->recv_buf[tail] = src[sent + i];
      tail = (tail + 1) % size;
    }

    peer->recv_buf_tail = tail;
    sent += to_copy;

    spinlock_release(&peer->recv_lock);

    // Wake up peer (they might be blocked on recv)
    wait_queue_wake_all(peer->wait);

    // Also wake poll() waiters on the peer socket's VFS node
    if (peer->parent && peer->parent->wait_queue) {
      wait_queue_wake_all((wait_queue_t *)peer->parent->wait_queue);
    }

    // Notify epoll watchers on peer socket that data is available
    if (peer->parent && peer->parent->node) {
      epoll_notify_event(peer->parent->node, EPOLLIN | EPOLLRDNORM);
    }
  }

  return (ssize_t)sent;
}

static ssize_t unix_recv(socket_t *sock, void *buf, size_t len, int flags) {
  if (!sock || !sock->sk)
    return -9; // EBADF

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  // For STREAM sockets, we can recv if connected or if we have data left but
  // peer disconnected
  if (sock->state != SS_CONNECTED && usk->recv_buf_head == usk->recv_buf_tail) {
    if (sock->state == SS_UNCONNECTED) {
      // For AF_UNIX, if the peer has closed, read should return 0 (EOF)
      // ENOTCONN is only for sockets that were NEVER connected.
      // Given our simple state machine, 0 is safer for X11 apps.
      return 0; // EOF
    }
    return 0; // EOF
  }

  uint8_t *dest = (uint8_t *)buf;
  size_t received = 0;

  while (received < len) {
    spinlock_acquire(&usk->recv_lock);

    size_t head = usk->recv_buf_head;
    size_t tail = usk->recv_buf_tail;
    size_t size = usk->recv_buf_size;
    size_t available = (tail - head + size) % size;

    if (available == 0) {
      spinlock_release(&usk->recv_lock);

      if (received > 0)
        break;

      // If peer disconnected, return 0 (EOF)
      if (sock->state != SS_CONNECTED) {
        // Clear accepted_orphaned flag so poll won't return POLLIN anymore
        usk->accepted_orphaned = false;
        return 0;
      }

      if ((sock->flags & SOCK_NONBLOCK) ||
          (flags & 0x40)) { // MSG_DONTWAIT is 0x40
        return -11;         // EAGAIN
      }

      // Block until data available
      struct thread *current = sched_get_current();
      wait_queue_entry_t entry;
      entry.thread = current;
      entry.next = NULL;

      wait_queue_add(usk->wait, &entry);
      current->state = THREAD_BLOCKED;

      // Re-check availability while BLOCKED
      head = usk->recv_buf_head;
      tail = usk->recv_buf_tail;
      size = usk->recv_buf_size;
      available = (tail - head + size) % size;

      if (available > 0 || sock->state != SS_CONNECTED) {
        current->state = THREAD_RUNNING;
        spinlock_release(&usk->recv_lock);
      } else {
        spinlock_release(&usk->recv_lock);
        sched_yield();
        spinlock_acquire(&usk->recv_lock);
      }

      wait_queue_remove(usk->wait, &entry);
      current->state = THREAD_RUNNING;

      if (sock->error)
        return -sock->error;
      continue;
    }

    size_t to_copy = (len - received < available) ? len - received : available;

    // Copy from ring buffer
    for (size_t i = 0; i < to_copy; i++) {
      dest[received + i] = usk->recv_buf[(head + i) % size];
    }

    if (!(flags & 0x02)) { // MSG_PEEK is 0x02
      usk->recv_buf_head = (head + to_copy) % size;
    }

    spinlock_release(&usk->recv_lock);
    received += to_copy;

    // For DGRAM, one recv() = one packet.
    // For STREAM, we can continue to fill the buffer if more data is available.
    if (sock->type == SOCK_DGRAM)
      break;
  }
  wait_queue_wake_all(usk->wait);

  // Notify peer that their send buffer has space (POLLOUT)
  if (usk->peer && usk->peer->parent && usk->peer->parent->node) {
    epoll_notify_event(usk->peer->parent->node, EPOLLOUT | EPOLLWRNORM);
  }

  return (ssize_t)received;
}

static ssize_t unix_sendto(socket_t *sock, const void *buf, size_t len,
                           int flags, struct sockaddr *dest_addr, int addrlen) {
  (void)addrlen; // Used for DGRAM sockets (Phase 5)
  if (!dest_addr) {
    return unix_send(sock, buf, len, flags);
  }
  // For DGRAM sockets, we'd find the peer by address here.
  // For STREAM, sendto with address is typically not supported if already
  // connected, or it behaves like connect+send.
  klog_puts(
      "[WARN] unix_sendto: with address not implementation (Phase 5 DGRAM)\n");
  return -95; // EOPNOTSUPP
}

static ssize_t unix_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                             struct sockaddr *src_addr, int *addrlen) {
  ssize_t ret = unix_recv(sock, buf, len, flags);
  if (ret >= 0 && src_addr && addrlen) {
    // Fill source address from peer
    unix_sock_t *usk = (unix_sock_t *)sock->sk;
    if (usk->peer) {
      int to_copy =
          (usk->peer->addr_len < *addrlen) ? usk->peer->addr_len : *addrlen;
      memcpy(src_addr, &usk->peer->addr, to_copy);
      *addrlen = to_copy;
    } else {
      *addrlen = 0;
    }
  }
  return ret;
}

static int unix_getsockopt(socket_t *sock, int level, int optname, void *optval,
                           int *optlen) {
  if (!sock || !sock->sk || !optval || !optlen)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  // Only handle SOL_SOCKET level here (family-specific options)
  if (level != SOL_SOCKET) {
    return -92; // ENOPROTOOPT
  }

  switch (optname) {
  case SO_ACCEPTCONN:
    // Return whether socket is listening
    if (*optlen < (int)sizeof(int))
      return -22; // EINVAL
    *(int *)optval = usk->is_listener ? 1 : 0;
    *optlen = sizeof(int);
    return 0;

  case SO_PASSCRED:
    if (*optlen < (int)sizeof(int))
      return -22;
    *(int *)optval = usk->passcred ? 1 : 0;
    *optlen = sizeof(int);
    return 0;

  case SO_PEERCRED: {
    // Return peer credentials (simplified - just PID for now)
    if (*optlen < (int)UCRED_SIZE) {
      klog_puts("[WARN] unix_getsockopt: SO_PEERCRED buffer too small\n");
      return -22;
    }
    if (!usk->peer) {
      klog_puts("[WARN] unix_getsockopt: SO_PEERCRED but no peer\n");
      return -107; // ENOTCONN
    }
    // Simplified credentials - in real OS would get from peer's process
    struct ucred_local *cred = (struct ucred_local *)optval;
    cred->pid = 1; // Placeholder
    cred->uid = 0;
    cred->gid = 0;
    *optlen = (int)UCRED_SIZE;
    return 0;
  }

  case SO_RCVTIMEO:
    if (*optlen < (int)sizeof(int))
      return -22;
    if (*optlen >= (int)TIMEVAL_SIZE) {
      struct timeval_local *tv = (struct timeval_local *)optval;
      tv->tv_sec = usk->rcvtimeo_ms / 1000;
      tv->tv_usec = (usk->rcvtimeo_ms % 1000) * 1000;
      *optlen = (int)TIMEVAL_SIZE;
    } else {
      *(int *)optval = usk->rcvtimeo_ms;
      *optlen = sizeof(int);
    }
    return 0;

  case SO_SNDTIMEO:
    if (*optlen < (int)sizeof(int))
      return -22;
    if (*optlen >= (int)TIMEVAL_SIZE) {
      struct timeval_local *tv = (struct timeval_local *)optval;
      tv->tv_sec = usk->sndtimeo_ms / 1000;
      tv->tv_usec = (usk->sndtimeo_ms % 1000) * 1000;
      *optlen = (int)TIMEVAL_SIZE;
    } else {
      *(int *)optval = usk->sndtimeo_ms;
      *optlen = sizeof(int);
    }
    return 0;

  default:
    klog_puts("[WARN] unix_getsockopt: unknown optname ");
    klog_uint64(optname);
    klog_puts("\n");
    return -92; // ENOPROTOOPT
  }
}

static int unix_setsockopt(socket_t *sock, int level, int optname,
                           const void *optval, int optlen) {
  if (!sock || !sock->sk || !optval)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  // Only handle SOL_SOCKET level here
  if (level != SOL_SOCKET) {
    return -92; // ENOPROTOOPT
  }

  switch (optname) {
  case SO_RCVBUF: {
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    // Clamp to reasonable limits
    if (val < 256)
      val = 256;
    if (val > 1024 * 1024)
      val = 1024 * 1024; // 1MB max

    // Try to reallocate buffer
    uint8_t *new_buf = kmalloc((size_t)val);
    if (!new_buf) {
      klog_puts("[WARN] unix_setsockopt: failed to allocate new recv buffer\n");
      return -12; // ENOMEM
    }

    spinlock_acquire(&usk->recv_lock);
    // Copy existing data if any
    size_t head = usk->recv_buf_head;
    size_t tail = usk->recv_buf_tail;
    size_t old_size = usk->recv_buf_size;
    size_t available = (tail - head + old_size) % old_size;

    // Copy data to new buffer
    for (size_t i = 0; i < available && i < (size_t)val; i++) {
      new_buf[i] = usk->recv_buf[(head + i) % old_size];
    }
    usk->recv_buf_head = 0;
    usk->recv_buf_tail = available < (size_t)val ? available : (size_t)val;

    kfree(usk->recv_buf);
    usk->recv_buf = new_buf;
    usk->recv_buf_size = (size_t)val;
    sock->rcvbuf = val;
    spinlock_release(&usk->recv_lock);

    klog_puts("[OK] unix_setsockopt: SO_RCVBUF set to ");
    klog_uint64(val);
    klog_puts("\n");
    return 0;
  }

  case SO_SNDBUF: {
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    if (val < 256)
      val = 256;
    if (val > 1024 * 1024)
      val = 1024 * 1024;

    uint8_t *new_buf = kmalloc((size_t)val);
    if (!new_buf) {
      klog_puts("[WARN] unix_setsockopt: failed to allocate new send buffer\n");
      return -12;
    }

    spinlock_acquire(&usk->send_lock);
    kfree(usk->send_buf);
    usk->send_buf = new_buf;
    usk->send_buf_size = (size_t)val;
    usk->send_buf_head = 0;
    usk->send_buf_tail = 0;
    sock->sndbuf = val;
    spinlock_release(&usk->send_lock);

    klog_puts("[OK] unix_setsockopt: SO_SNDBUF set to ");
    klog_uint64(val);
    klog_puts("\n");
    return 0;
  }

  case SO_RCVBUFFORCE: {
    // Like SO_RCVBUF but bypass limits (still need memory)
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    if (val < 64)
      val = 64; // Minimum

    uint8_t *new_buf = kmalloc((size_t)val);
    if (!new_buf)
      return -12;

    spinlock_acquire(&usk->recv_lock);
    kfree(usk->recv_buf);
    usk->recv_buf = new_buf;
    usk->recv_buf_size = (size_t)val;
    usk->recv_buf_head = 0;
    usk->recv_buf_tail = 0;
    sock->rcvbuf = val;
    spinlock_release(&usk->recv_lock);
    return 0;
  }

  case SO_SNDBUFFORCE: {
    if (optlen < (int)sizeof(int))
      return -22;
    int val = *(const int *)optval;
    if (val < 64)
      val = 64;

    uint8_t *new_buf = kmalloc((size_t)val);
    if (!new_buf)
      return -12;

    spinlock_acquire(&usk->send_lock);
    kfree(usk->send_buf);
    usk->send_buf = new_buf;
    usk->send_buf_size = (size_t)val;
    usk->send_buf_head = 0;
    usk->send_buf_tail = 0;
    sock->sndbuf = val;
    spinlock_release(&usk->send_lock);
    return 0;
  }

  case SO_PASSCRED:
    if (optlen < (int)sizeof(int))
      return -22;
    usk->passcred = (*(const int *)optval != 0);
    klog_puts("[OK] unix_setsockopt: SO_PASSCRED set to ");
    klog_uint64(usk->passcred ? 1 : 0);
    klog_puts("\n");
    return 0;

  case SO_RCVTIMEO: {
    if (optlen < (int)sizeof(int))
      return -22;
    // Accept either int (ms) or struct timeval
    if (optlen >= (int)TIMEVAL_SIZE) {
      const struct timeval_local *tv = (const struct timeval_local *)optval;
      usk->rcvtimeo_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    } else {
      usk->rcvtimeo_ms = *(const int *)optval;
    }
    klog_puts("[OK] unix_setsockopt: SO_RCVTIMEO set to ");
    klog_uint64(usk->rcvtimeo_ms);
    klog_puts(" ms\n");
    return 0;
  }

  case SO_SNDTIMEO: {
    if (optlen < (int)sizeof(int))
      return -22;
    if (optlen >= (int)TIMEVAL_SIZE) {
      const struct timeval_local *tv = (const struct timeval_local *)optval;
      usk->sndtimeo_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    } else {
      usk->sndtimeo_ms = *(const int *)optval;
    }
    klog_puts("[OK] unix_setsockopt: SO_SNDTIMEO set to ");
    klog_uint64(usk->sndtimeo_ms);
    klog_puts(" ms\n");
    return 0;
  }

  default:
    klog_puts("[WARN] unix_setsockopt: unknown optname ");
    klog_uint64(optname);
    klog_puts("\n");
    return -92; // ENOPROTOOPT
  }
}

static int unix_shutdown(socket_t *sock, int how) {
  if (!sock || !sock->sk)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  if (sock->state != SS_CONNECTED) {
    klog_puts("[WARN] unix_shutdown: socket not connected\n");
    return -107; // ENOTCONN
  }

  unix_sock_t *peer = usk->peer;
  if (!peer) {
    klog_puts("[WARN] unix_shutdown: no peer\n");
    return -107; // ENOTCONN
  }

  // Handle shutdown modes
  switch (how) {
  case SHUT_RD:
    // Disallow further receives
    klog_puts("[OK] unix_shutdown: SHUT_RD\n");
    usk->read_shutdown = true;
    break;

  case SHUT_WR:
    // Disallow further sends - notify peer
    klog_puts("[OK] unix_shutdown: SHUT_WR\n");
    usk->write_shutdown = true;
    // Wake up peer so they can detect EOF
    wait_queue_wake_all(peer->wait);
    // Notify peer's epoll watchers that write side is closed (EPOLLRDHUP)
    if (peer->parent && peer->parent->node) {
      epoll_notify_event(peer->parent->node, EPOLLIN | EPOLLHUP | EPOLLRDHUP);
    }
    break;

  case SHUT_RDWR:
    klog_puts("[OK] unix_shutdown: SHUT_RDWR\n");
    usk->read_shutdown = true;
    usk->write_shutdown = true;
    wait_queue_wake_all(peer->wait);
    // Notify peer of full shutdown
    if (peer->parent && peer->parent->node) {
      epoll_notify_event(peer->parent->node, EPOLLIN | EPOLLHUP | EPOLLRDHUP);
    }
    break;

  default:
    klog_puts("[WARN] unix_shutdown: invalid how=");
    klog_uint64(how);
    klog_puts("\n");
    return -22; // EINVAL
  }

  return 0;
}

static int unix_poll(socket_t *sock, int events) {
  if (!sock || !sock->sk)
    return 0;

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  int revents = 0;

  // Check for errors
  if (sock->error)
    revents |= 0x008; // POLLERR

  // Check for readable data first
  spinlock_acquire(&usk->recv_lock);
  size_t head = usk->recv_buf_head;
  size_t tail = usk->recv_buf_tail;
  size_t size = usk->recv_buf_size;
  size_t available = (tail - head + size) % size;
  spinlock_release(&usk->recv_lock);

  if (available > 0) {
    revents |= 0x001; // POLLIN
    revents |= 0x040; // POLLRDNORM
  }

  // Check for hangup - peer closed or shutdown
  if (sock->state == SS_DISCONNECTING || sock->state == SS_UNCONNECTED ||
      (usk->read_shutdown && usk->write_shutdown) ||
      (usk->peer == NULL && sock->state != SS_LISTENING)) {
    revents |= 0x010;  // POLLHUP
    revents |= 0x2000; // EPOLLRDHUP - peer closed
    // POSIX: POLLIN should be set on hangup so read() returns EOF (0)
    revents |= 0x001; // POLLIN
  }

  // If peer did shutdown(SHUT_WR), treat as hangup
  // Also report EPOLLRDHUP for peer write-side close
  if (usk->peer && usk->peer->write_shutdown && available == 0) {
    revents |= 0x010;  // POLLHUP
    revents |= 0x2000; // EPOLLRDHUP - peer closed write side
    // Note: POLLIN for EOF not set - use POLLHUP/EPOLLRDHUP to detect peer
    // close
  }

  // Check for writability (connected, peer exists, and not shutdown for write)
  if (sock->state == SS_CONNECTED && usk->peer && !usk->write_shutdown) {
    revents |= 0x004; // POLLOUT
    revents |= 0x100; // POLLWRNORM
  }

  // For listeners, check for pending connections
  if (usk->is_listener && usk->accept_next) {
    revents |= 0x001; // POLLIN
  }

  return revents & events;
}

// ── AF_UNIX Operations Vector
// ──────────────────────────────────────────────────

static int unix_ioctl(socket_t *sock, uint32_t request, uint64_t arg) {
  if (!sock || !sock->sk)
    return -22;
  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  switch (request) {
  case 0x541B: { // FIONREAD
    int *val = (int *)arg;
    if (!val)
      return -14; // EFAULT
    spinlock_acquire(&usk->recv_lock);
    size_t head = usk->recv_buf_head;
    size_t tail = usk->recv_buf_tail;
    size_t size = usk->recv_buf_size;
    size_t available = (tail - head + size) % size;
    *val = (int)available;
    spinlock_release(&usk->recv_lock);
    return 0;
  }
  case 0x5421: { // FIONBIO
    int *val = (int *)arg;
    if (!val)
      return -14;
    if (*val) {
      sock->flags |= SOCK_NONBLOCK;
    } else {
      sock->flags &= ~SOCK_NONBLOCK;
    }
    return 0;
  }
  default:
    return -25; // ENOTTY
  }
}

static ssize_t unix_sendmsg(socket_t *sock, struct msghdr *msg, int flags) {
  if (!sock || !msg)
    return -22; // EINVAL
  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;

  spinlock_acquire(&sock->lock);
  unix_sock_t *peer = usk->peer;
  if (!peer) {
    spinlock_release(&sock->lock);
    return -107; // ENOTCONN
  }
  socket_get(peer->parent); // Keep peer alive
  spinlock_release(&sock->lock);

  struct thread *current = sched_get_current();

  // 1. Handle SCM_RIGHTS (FD passing)
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
  while (cmsg) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      int *fds = (int *)CMSG_DATA(cmsg);
      int count = (cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) / sizeof(int);

      spinlock_acquire(&peer->parent->lock);
      for (int i = 0; i < count && peer->scm_count < 16; i++) {
        int fd = fds[i];
        if (fd >= 0 && fd < MAX_FDS && current->fds[fd]) {
          vfs_node_t *node = current->fds[fd];
          vfs_open(node); // Increment refcount
          peer->scm_nodes[peer->scm_count++] = node;
        }
      }
      spinlock_release(&peer->parent->lock);
    }
    cmsg = CMSG_NXTHDR(msg, cmsg);
  }

  // 2. Handle data (iovec)
  ssize_t total_sent = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    ssize_t ret =
        unix_send(sock, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len, flags);
    if (ret < 0) {
      socket_put(peer->parent);
      return total_sent > 0 ? total_sent : ret;
    }
    total_sent += ret;
  }

  socket_put(peer->parent);
  return total_sent;
}

static ssize_t unix_recvmsg(socket_t *sock, struct msghdr *msg, int flags) {
  if (!sock || !msg)
    return -22; // EINVAL
  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;

  struct thread *current = sched_get_current();

  // 1. Handle data (iovec)
  ssize_t total_received = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    ssize_t ret =
        unix_recv(sock, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len, flags);
    if (ret < 0) {
      return total_received > 0 ? total_received : ret;
    }
    total_received += ret;
  }

  // 2. Handle SCM_RIGHTS (received FDs)
  spinlock_acquire(&sock->lock);
  if (usk->scm_count > 0 && msg->msg_control &&
      msg->msg_controllen >= CMSG_SPACE(usk->scm_count * sizeof(int))) {
    struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(usk->scm_count * sizeof(int));

    int *fds = (int *)CMSG_DATA(cmsg);
    int actual_count = 0;
    for (int i = 0; i < usk->scm_count; i++) {
      vfs_node_t *node = usk->scm_nodes[i];
      int new_fd = alloc_fd(current);
      if (new_fd >= 0) {
        current->fds[new_fd] = node;
        // vfs_open was already called in sendmsg
        fds[actual_count++] = new_fd;
      } else {
        vfs_close(node); // Drop reference if we can't give it to process
      }
      usk->scm_nodes[i] = NULL;
    }
    msg->msg_controllen = CMSG_SPACE(actual_count * sizeof(int));
    usk->scm_count = 0;
  } else if (usk->scm_count > 0) {
    msg->msg_flags |= MSG_CTRUNC;
  }
  spinlock_release(&sock->lock);

  return total_received;
}

static sock_ops_t unix_ops = {.bind = unix_bind,
                              .connect = unix_connect,
                              .listen = unix_listen,
                              .accept = unix_accept,
                              .send = unix_send,
                              .recv = unix_recv,
                              .sendto = unix_sendto,
                              .recvfrom = unix_recvfrom,
                              .sendmsg = unix_sendmsg,
                              .recvmsg = unix_recvmsg,
                              .getsockopt = unix_getsockopt,
                              .setsockopt = unix_setsockopt,
                              .shutdown = unix_shutdown,
                              .poll = unix_poll,
                              .ioctl = unix_ioctl,
                              .destroy = unix_destroy};

// ── AF_UNIX Socket Creation
// ────────────────────────────────────────────────────

int unix_create(socket_t *sock, int protocol) {
  if (!sock) {
    return -22; // EINVAL
  }

  // Protocol validation for AF_UNIX
  // AF_UNIX does not support protocol differentiation, so protocol must be 0
  if (protocol != 0) {
    klog_puts("[WARN] unix_create: protocol must be 0 for AF_UNIX (got ");
    klog_uint64(protocol);
    klog_puts(")\n");
    return -EPROTONOSUPPORT;
  }

  // Type validation is already done in socket_create(), but double-check here
  if (sock->type != SOCK_STREAM && sock->type != SOCK_DGRAM &&
      sock->type != SOCK_SEQPACKET) {
    klog_puts("[WARN] unix_create: unsupported socket type ");
    klog_uint64(sock->type);
    klog_puts(" for AF_UNIX\n");
    return -EPROTONOSUPPORT;
  }

  // Allocate family-specific data (unix_sock_t)
  unix_sock_t *usk = kmalloc(sizeof(unix_sock_t));
  if (!usk) {
    klog_puts("[ERR] unix_create: failed to allocate unix_sock_t\n");
    return -12; // ENOMEM
  }

  // Initialize unix_sock_t
  memset(usk, 0, sizeof(unix_sock_t));

  // Link back to parent
  usk->parent = sock;
  usk->peer = NULL;
  usk->listener = NULL;
  usk->backlog = 0;
  usk->accept_queue_len = 0;
  usk->accept_next = NULL;
  usk->is_listener = false;
  usk->is_accepted = false;
  usk->is_abstract = false;

  // Initialize shutdown flags
  usk->read_shutdown = false;
  usk->write_shutdown = false;

  // Initialize socket options (Phase 6)
  usk->passcred = false;
  usk->rcvtimeo_ms = 0; // No timeout by default
  usk->sndtimeo_ms = 0; // No timeout by default

  // Initialize address
  usk->addr.sun_family = AF_UNIX;
  usk->addr.sun_path[0] = '\0';
  usk->addr_len = 0;

  // Use parent's wait queue for all blocking operations and poll()
  usk->wait = (wait_queue_t *)sock->wait_queue;

  // Initialize list node
  INIT_LIST_HEAD(&usk->bind_node);

  // Initialize receive buffer
  usk->recv_buf = kmalloc(sock->rcvbuf);
  if (!usk->recv_buf) {
    kfree(usk);
    klog_puts("[ERR] unix_create: failed to allocate receive buffer\n");
    return -12; // ENOMEM
  }
  usk->recv_buf_size = sock->rcvbuf;
  usk->recv_buf_head = 0;
  usk->recv_buf_tail = 0;
  spinlock_init(&usk->recv_lock);

  // Initialize send buffer
  usk->send_buf = kmalloc(sock->sndbuf);
  if (!usk->send_buf) {
    kfree(usk->recv_buf);
    kfree(usk);
    klog_puts("[ERR] unix_create: failed to allocate send buffer\n");
    return -12; // ENOMEM
  }
  usk->send_buf_size = sock->sndbuf;
  usk->send_buf_head = 0;
  usk->send_buf_tail = 0;
  spinlock_init(&usk->send_lock);

  // Link socket to family-specific data
  sock->sk = usk;
  sock->ops = &unix_ops;

  // Log successful creation
  klog_puts("[OK] AF_UNIX socket created (type=");
  if (sock->type == SOCK_STREAM) {
    klog_puts("SOCK_STREAM");
  } else if (sock->type == SOCK_DGRAM) {
    klog_puts("SOCK_DGRAM");
  } else if (sock->type == SOCK_SEQPACKET) {
    klog_puts("SOCK_SEQPACKET");
  } else {
    klog_puts("unknown");
  }
  klog_puts(")\n");

  return 0;
}

// ── AF_UNIX Socket Destruction
// ─────────────────────────────────────────────────

void unix_destroy(socket_t *sock) {
  if (!sock) {
    return;
  }

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk) {
    return;
  }

  klog_puts("[OK] Destroying AF_UNIX socket\n");

  // Remove from bound list if bound
  spinlock_acquire(&unix_bound_lock);
  if (!list_empty(&usk->bind_node)) {
    list_del(&usk->bind_node);
  }
  spinlock_release(&unix_bound_lock);

  // If filesystem socket, we should probably unlink it?
  // Actually, close() doesn't usually unlink the socket file.
  // The user should call unlink().

  // Free receive buffer
  if (usk->recv_buf) {
    kfree(usk->recv_buf);
    usk->recv_buf = NULL;
  }

  // Free send buffer
  if (usk->send_buf) {
    kfree(usk->send_buf);
    usk->send_buf = NULL;
  }

  // If listener, destroy all pending connections
  if (usk->is_listener) {
    unix_sock_t *curr = usk->accept_next;
    while (curr) {
      unix_sock_t *next = curr->accept_next;
      // The pending socket was created in unix_connect but never accepted.
      // We should destroy its parent socket.
      socket_put(curr->parent);
      curr = next;
    }
    usk->accept_next = NULL;
  }

  // Notify peer and wake up any waiters
  if (usk->peer) {
    unix_sock_t *peer = usk->peer;
    // Safely notify peer
    spinlock_acquire(&peer->parent->lock);
    peer->peer = NULL;
    if (peer->parent) {
      peer->parent->state = SS_UNCONNECTED;
      peer->parent->error = 104; // ECONNRESET
    }
    spinlock_release(&peer->parent->lock);

    wait_queue_wake_all(peer->wait);

    // Also wake poll() waiters on peer's VFS node
    if (peer->parent && peer->parent->wait_queue) {
      wait_queue_wake_all((wait_queue_t *)peer->parent->wait_queue);
    }

    // Notify peer's epoll watchers that connection is closed
    if (peer->parent && peer->parent->node) {
      epoll_notify_event(peer->parent->node, EPOLLIN | EPOLLHUP | EPOLLRDHUP);
    }
    usk->peer = NULL;
  }

  // Wake up anyone waiting on our own queue (like senders)
  wait_queue_wake_all(usk->wait);

  // Also wake poll() waiters on our own VFS node
  if (sock->wait_queue) {
    wait_queue_wake_all((wait_queue_t *)sock->wait_queue);
  }

  // If we're in a listener's accept queue, don't free yet
  // Mark as orphaned - accept() will handle and free it
  if (usk->listener) {
    usk->orphaned = true; // Mark for accept() to clean up
    usk->parent = NULL;
    // Keep in queue but mark as orphaned
    // Don't free - accept() will do it
    sock->sk = NULL;
    return;
  }

  // Free the unix_sock_t structure
  kfree(usk);
  sock->sk = NULL;
}

// ── AF_UNIX Family Registration
// ────────────────────────────────────────────────

net_family_t *unix_get_family(void) { return &unix_family; }

sock_ops_t *unix_get_ops(void) { return &unix_ops; }

void af_unix_init(void) {
  // Initialize bound sockets list and lock
  INIT_LIST_HEAD(&unix_bound_list);
  spinlock_init(&unix_bound_lock);

  // Register the AF_UNIX family
  sock_register_family(&unix_family);

  klog_puts("[OK] AF_UNIX socket family registered\n");
}
