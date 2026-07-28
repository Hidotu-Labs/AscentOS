// AF_UNIX Socket Family – lifecycle, ops vector, and registration
//
// I/O         → af_unix_io.c
// Bind        → af_unix_bind.c
// Connect     → af_unix_connect.c
// Socket opts → af_unix_sockopt.c
// Address reg → af_unix_addr.c

#include "af_unix_internal.h"

// ── Forward declarations from sub-modules ────────────────────────────────────

// af_unix_bind.c
int unix_bind_impl(socket_t *sock, struct sockaddr *addr, int addrlen);

// af_unix_connect.c
int unix_listen_impl(socket_t *sock, int backlog);
int unix_connect_impl(socket_t *sock, struct sockaddr *addr, int addrlen);
int unix_accept_impl(socket_t *sock, socket_t **newsock);

// af_unix_io.c
ssize_t unix_send_impl(socket_t *sock, const void *buf, size_t len, int flags);
ssize_t unix_recv_impl(socket_t *sock, void *buf, size_t len, int flags);
ssize_t unix_sendto_impl(socket_t *sock, const void *buf, size_t len,
                         int flags, struct sockaddr *dest_addr, int addrlen);
ssize_t unix_recvfrom_impl(socket_t *sock, void *buf, size_t len, int flags,
                           struct sockaddr *src_addr, int *addrlen);
ssize_t unix_sendmsg_impl(socket_t *sock, struct msghdr *msg, int flags);
ssize_t unix_recvmsg_impl(socket_t *sock, struct msghdr *msg, int flags);

// af_unix_sockopt.c
int unix_getsockopt_impl(socket_t *sock, int level, int optname,
                         void *optval, int *optlen);
int unix_setsockopt_impl(socket_t *sock, int level, int optname,
                         const void *optval, int optlen);
int unix_getsockname_impl(socket_t *sock, struct sockaddr *addr, int *addrlen);
int unix_getpeername_impl(socket_t *sock, struct sockaddr *addr, int *addrlen);

// ── Local types (SO_PEERCRED / SO_RCVTIMEO helpers not needed here) ──────────

// ── Shutdown ─────────────────────────────────────────────────────────────────

static int unix_shutdown(socket_t *sock, int how) {
  if (!sock || !sock->sk)
    return -22; // EINVAL

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  // Only sockets that were ever connected can be shut down.
  // was_connected is set when SS_CONNECTED is first reached and never cleared,
  // so it survives peer teardown (which resets state -> SS_UNCONNECTED and
  // peer -> NULL). A listening socket or a freshly-created socket that never
  // completed a connect gets ENOTCONN; a socket whose peer already called
  // close() succeeds as a no-op (matching Linux behaviour).
  if (!usk->was_connected) {
    klog_puts("[WARN] unix_shutdown: socket not connected\n");
    return -107; // ENOTCONN
  }

  if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
    klog_puts("[WARN] unix_shutdown: invalid how=");
    klog_uint64(how);
    klog_puts("\n");
    return -22; // EINVAL
  }

  socket_t *peer_sock = NULL;
  spinlock_acquire(&sock->lock);
  unix_sock_t *peer = usk->peer; // may be NULL if peer already closed
  if (peer && peer->parent && socket_try_get(peer->parent))
    peer_sock = peer->parent;
  spinlock_release(&sock->lock);

  switch (how) {
  case SHUT_RD:
    klog_puts("[OK] unix_shutdown: SHUT_RD\n");
    usk->read_shutdown = true;
    break;

  case SHUT_WR:
    klog_puts("[OK] unix_shutdown: SHUT_WR\n");
    usk->write_shutdown = true;
    break;

  case SHUT_RDWR:
    klog_puts("[OK] unix_shutdown: SHUT_RDWR\n");
    usk->read_shutdown  = true;
    usk->write_shutdown = true;
    break;
  }

  if (peer_sock) {
    peer = (unix_sock_t *)peer_sock->sk;
    if (peer && peer->wait)
      wait_queue_wake_all(peer->wait);
    if (peer_sock->node)
      epoll_notify_event(peer_sock->node, EPOLLIN | EPOLLHUP | EPOLLRDHUP);
    socket_put(peer_sock);
  }

  return 0;
}

// ── Poll ─────────────────────────────────────────────────────────────────────

static int unix_poll(socket_t *sock, int events) {
  if (!sock || !sock->sk)
    return 0;

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  int revents = 0;

  if (sock->error)
    revents |= 0x008; // POLLERR

  spinlock_acquire(&usk->recv_lock);
  size_t head      = usk->recv_buf_head;
  size_t tail      = usk->recv_buf_tail;
  size_t size      = usk->recv_buf_size;
  size_t available = (size > 0) ? (tail - head + size) % size : 0;
  spinlock_release(&usk->recv_lock);

  bool has_peer = false;
  bool peer_write_shutdown = false;
  spinlock_acquire(&sock->lock);
  unix_sock_t *peer = usk->peer;
  if (peer) {
    has_peer = true;
    peer_write_shutdown = peer->write_shutdown;
  }
  spinlock_release(&sock->lock);

  if (available > 0) {
    revents |= 0x001; // POLLIN
    revents |= 0x040; // POLLRDNORM
  }

  if (sock->state == SS_DISCONNECTING || sock->state == SS_UNCONNECTED ||
      (usk->read_shutdown && usk->write_shutdown) ||
      (!has_peer && sock->state != SS_LISTENING)) {
    revents |= 0x010;  // POLLHUP
    revents |= 0x2000; // EPOLLRDHUP
    revents |= 0x001;  // POLLIN (EOF)
  }

  if (has_peer && peer_write_shutdown && available == 0) {
    revents |= 0x010;  // POLLHUP
    revents |= 0x2000; // EPOLLRDHUP
  }

  if (sock->state == SS_CONNECTED && has_peer && !usk->write_shutdown) {
    revents |= 0x004; // POLLOUT
    revents |= 0x100; // POLLWRNORM
  }

  if (usk->is_listener && usk->accept_next)
    revents |= 0x001; // POLLIN

  return revents & events;
}

// ── ioctl ────────────────────────────────────────────────────────────────────

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
    size_t head      = usk->recv_buf_head;
    size_t tail      = usk->recv_buf_tail;
    size_t sz        = usk->recv_buf_size;
    size_t available = (sz > 0) ? (tail - head + sz) % sz : 0;
    *val = (int)available;
    spinlock_release(&usk->recv_lock);
    return 0;
  }
  case 0x5421: { // FIONBIO
    int *val = (int *)arg;
    if (!val)
      return -14;
    if (*val)
      sock->flags |= SOCK_NONBLOCK;
    else
      sock->flags &= ~SOCK_NONBLOCK;
    return 0;
  }
  default:
    return -25; // ENOTTY
  }
}

// ── Operations vector ────────────────────────────────────────────────────────

static sock_ops_t unix_ops = {
    .bind        = unix_bind_impl,
    .connect     = unix_connect_impl,
    .listen      = unix_listen_impl,
    .accept      = unix_accept_impl,
    .send        = unix_send_impl,
    .recv        = unix_recv_impl,
    .sendto      = unix_sendto_impl,
    .recvfrom    = unix_recvfrom_impl,
    .sendmsg     = unix_sendmsg_impl,
    .recvmsg     = unix_recvmsg_impl,
    .getsockopt  = unix_getsockopt_impl,
    .setsockopt  = unix_setsockopt_impl,
    .getsockname = unix_getsockname_impl,
    .getpeername = unix_getpeername_impl,
    .shutdown    = unix_shutdown,
    .poll        = unix_poll,
    .ioctl       = unix_ioctl,
    .destroy     = unix_destroy,
};

// ── Family structure ─────────────────────────────────────────────────────────

static net_family_t unix_family = {
    .family = AF_UNIX,
    .create = unix_create,
    .next   = NULL,
};

// ── Creation ─────────────────────────────────────────────────────────────────

int unix_create(socket_t *sock, int protocol) {
  if (!sock)
    return -22; // EINVAL

  if (protocol != 0) {
    klog_puts("[WARN] unix_create: protocol must be 0 for AF_UNIX (got ");
    klog_uint64(protocol);
    klog_puts(")\n");
    return -EPROTONOSUPPORT;
  }

  if (sock->type != SOCK_STREAM && sock->type != SOCK_DGRAM &&
      sock->type != SOCK_SEQPACKET) {
    klog_puts("[WARN] unix_create: unsupported socket type ");
    klog_uint64(sock->type);
    klog_puts(" for AF_UNIX\n");
    return -EPROTONOSUPPORT;
  }

  unix_sock_t *usk = kmalloc(sizeof(unix_sock_t));
  if (!usk) {
    klog_puts("[ERR] unix_create: failed to allocate unix_sock_t\n");
    return -12; // ENOMEM
  }

  memset(usk, 0, sizeof(unix_sock_t));

  struct thread *owner = sched_get_current();
  if (owner) {
    usk->owner_pid = owner->tgid;
    usk->owner_uid = owner->euid;
    usk->owner_gid = owner->egid;
  }

  usk->parent           = sock;
  usk->peer             = NULL;
  usk->listener         = NULL;
  usk->backlog          = 0;
  usk->accept_queue_len = 0;
  usk->accept_next      = NULL;
  usk->is_listener      = false;
  usk->is_accepted      = false;
  usk->is_abstract      = false;
  usk->read_shutdown    = false;
  usk->write_shutdown   = false;
  usk->orphaned         = false;
  usk->accepted_orphaned = false;
  usk->was_connected    = false;
  usk->passcred         = false;
  usk->rcvtimeo_ms      = 0;
  usk->sndtimeo_ms      = 0;

  usk->addr.sun_family  = AF_UNIX;
  usk->addr.sun_path[0] = '\0';
  usk->addr_len         = 0;

  usk->wait = (wait_queue_t *)sock->wait_queue;

  INIT_LIST_HEAD(&usk->bind_node);

  // Receive buffer (lazy allocation on demand)
  usk->recv_buf = NULL;
  usk->recv_buf_size = 0;
  usk->recv_buf_head = 0;
  usk->recv_buf_tail = 0;
  spinlock_init(&usk->recv_lock);

  // Send buffer (lazy allocation on demand)
  usk->send_buf = NULL;
  usk->send_buf_size = 0;
  usk->send_buf_head = 0;
  usk->send_buf_tail = 0;
  spinlock_init(&usk->send_lock);

  sock->sk  = usk;
  sock->ops = &unix_ops;

  return 0;
}

// ── Destruction ──────────────────────────────────────────────────────────────

void unix_destroy(socket_t *sock) {
  if (!sock)
    return;

  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return;

  // Remove from bound list.  New lookups cannot find this socket after this.
  spinlock_acquire(&unix_bound_lock);
  if (!list_empty(&usk->bind_node))
    list_del(&usk->bind_node);
  spinlock_release(&unix_bound_lock);

  // Detach the peer before freeing buffers so concurrent I/O stops seeing us.
  if (usk->peer) {
    unix_sock_t *peer = usk->peer;
    socket_t *peer_parent = peer->parent;

    if (peer_parent) {
      spinlock_acquire(&peer_parent->lock);
      if (peer->peer == usk)
        peer->peer = NULL;
      peer_parent->state = SS_UNCONNECTED;
      peer_parent->error = 104; // ECONNRESET
      spinlock_release(&peer_parent->lock);

      if (peer_parent->wait_queue)
        wait_queue_wake_all((wait_queue_t *)peer_parent->wait_queue);
      if (peer_parent->node)
        epoll_notify_event(peer_parent->node, EPOLLIN | EPOLLHUP | EPOLLRDHUP);
    }

    wait_queue_wake_all(peer->wait);
    usk->peer = NULL;
  }

  // Destroy pending accept-queue entries.
  if (usk->is_listener) {
    unix_sock_t *curr = usk->accept_next;
    while (curr) {
      unix_sock_t *next = curr->accept_next;
      socket_put(curr->parent);
      curr = next;
    }
    usk->accept_next = NULL;
  }

  for (int i = 0; i < usk->scm_count; i++) {
    if (usk->scm_nodes[i]) {
      vfs_close(usk->scm_nodes[i]);
      usk->scm_nodes[i] = NULL;
    }
  }
  usk->scm_count = 0;

  if (usk->recv_buf) {
    kfree(usk->recv_buf);
    usk->recv_buf = NULL;
  }

  if (usk->send_buf) {
    kfree(usk->send_buf);
    usk->send_buf = NULL;
  }

  wait_queue_wake_all(usk->wait);

  if (sock->wait_queue)
    wait_queue_wake_all((wait_queue_t *)sock->wait_queue);

  // If still in a listener's queue, mark as orphaned and let accept() free it
  if (usk->listener) {
    usk->orphaned = true;
    usk->parent   = NULL;
    sock->sk      = NULL;
    return;
  }

  kfree(usk);
  sock->sk = NULL;
}

// ── Public accessors ─────────────────────────────────────────────────────────

net_family_t *unix_get_family(void) { return &unix_family; }
sock_ops_t   *unix_get_ops(void)    { return &unix_ops; }

// ── Initialisation ───────────────────────────────────────────────────────────

void af_unix_init(void) {
  INIT_LIST_HEAD(&unix_bound_list);
  spinlock_init(&unix_bound_lock);

  sock_register_family(&unix_family);

  klog_puts("[OK] AF_UNIX socket family registered\n");
}
