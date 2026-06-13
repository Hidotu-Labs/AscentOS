// AF_UNIX – Listen / connect / accept

#include "af_unix_internal.h"

int unix_listen_impl(socket_t *sock, int backlog) {
  if (!sock)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;

  spinlock_acquire(&sock->lock);

  usk->is_listener    = true;
  usk->backlog        = (backlog > 0) ? backlog : 128;
  usk->accept_queue_len = 0;
  usk->accept_next    = NULL;
  sock->state         = SS_LISTENING;

  spinlock_release(&sock->lock);

  klog_puts("[OK] unix_listen: socket is now listening (backlog=");
  klog_uint64(usk->backlog);
  klog_puts(")\n");
  return 0;
}

int unix_connect_impl(socket_t *sock, struct sockaddr *addr, int addrlen) {
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

  // Create the server-side socket early so the peer link is established
  // before accept() is called.
  socket_t *server_sock =
      socket_create(sock->domain, sock->type, sock->protocol);
  if (!server_sock)
    return -12; // ENOMEM

  unix_sock_t *server_usk = (unix_sock_t *)server_sock->sk;

  usk->peer        = server_usk;
  server_usk->peer = usk;

  sock->state        = SS_CONNECTED;
  server_sock->state = SS_CONNECTED;
  usk->was_connected        = true;
  server_usk->was_connected = true;

  spinlock_acquire(&dusk->parent->lock);

  if (dusk->accept_queue_len >= dusk->backlog) {
    spinlock_release(&dusk->parent->lock);
    klog_puts("[WARN] unix_connect: listener backlog full\n");
    usk->peer        = NULL;
    server_usk->peer = NULL;
    socket_put(server_sock);
    return -111; // ECONNREFUSED
  }

  // Enqueue server socket in listener's accept queue
  server_usk->accept_next = NULL;
  if (dusk->accept_next == NULL) {
    dusk->accept_next = server_usk;
  } else {
    unix_sock_t *curr = dusk->accept_next;
    while (curr->accept_next)
      curr = curr->accept_next;
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

int unix_accept_impl(socket_t *sock, socket_t **newsock) {
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
    entry.next   = NULL;

    wait_queue_add(usk->wait, &entry);
    current->state = THREAD_BLOCKED;

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

  unix_sock_t *new_usk   = usk->accept_next;
  usk->accept_next       = new_usk->accept_next;
  usk->accept_queue_len--;

  spinlock_release(&sock->lock);

  socket_t *new_sock    = new_usk->parent;
  new_usk->is_accepted  = true;

  *newsock = new_sock;

  klog_puts("[OK] unix_accept: retrieved early-linked connection\n");
  return 0;
}
