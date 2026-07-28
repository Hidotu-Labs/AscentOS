// AF_UNIX – I/O: send / recv / sendto / recvfrom / sendmsg / recvmsg

#include "af_unix_internal.h"
#include "../apic/lapic_timer.h"

#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL

static bool unix_user_range_valid(uint64_t addr, size_t len) {
  if (!addr)
    return false;
  if (addr > USER_ADDR_MAX)
    return false;
  if (len && addr + len - 1 > USER_ADDR_MAX)
    return false;
  return vmm_is_user_addr_range_valid(addr, len);
}

struct unix_ucred {
  int pid;
  int uid;
  int gid;
};

// Called with peer->recv_lock held. Linux attaches the credentials of the
// sending process to data received on an SO_PASSCRED socket, including data
// sent through write(2)/send(2), not only explicit sendmsg(2) control data.
static void unix_record_sender_credentials(unix_sock_t *peer,
                                           struct thread *sender) {
  if (!peer->passcred || !sender)
    return;

  peer->scm_cred_pid = (int)sender->tgid;
  peer->scm_cred_uid = (int)sender->uid;
  peer->scm_cred_gid = (int)sender->gid;
  peer->scm_cred_pending = true;
}

static socket_t *unix_get_live_peer(socket_t *sock, unix_sock_t **peer_out) {
  if (peer_out)
    *peer_out = NULL;
  if (!sock || !peer_out)
    return NULL;

  spinlock_acquire(&sock->lock);
  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  unix_sock_t *peer = usk ? usk->peer : NULL;
  socket_t *peer_sock = peer ? peer->parent : NULL;

  if (!peer_sock || !socket_try_get(peer_sock)) {
    spinlock_release(&sock->lock);
    return NULL;
  }

  if (peer_sock->closing || !peer_sock->sk) {
    spinlock_release(&sock->lock);
    socket_put(peer_sock);
    return NULL;
  }

  *peer_out = (unix_sock_t *)peer_sock->sk;
  spinlock_release(&sock->lock);
  return peer_sock;
}

static bool unix_ensure_recv_buf(unix_sock_t *usk) {
  if (!usk) return false;
  if (!usk->recv_buf) {
    size_t sz = (usk->parent && usk->parent->rcvbuf > 0) ? usk->parent->rcvbuf : 65536;
    usk->recv_buf = kmalloc(sz);
    if (!usk->recv_buf) return false;
    usk->recv_buf_size = sz;
    usk->recv_buf_head = 0;
    usk->recv_buf_tail = 0;
  }
  return true;
}

ssize_t unix_send_impl(socket_t *sock, const void *buf, size_t len, int flags) {
  (void)flags;
  if (!sock || !sock->sk)
    return -9; // EBADF

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  if (sock->closing)
    return -32; // EPIPE

  if (sock->state != SS_CONNECTED && sock->state != SS_CONNECTING)
    return -107; // ENOTCONN

  // If still CONNECTING, block until peer is set by accept()
  while (usk->peer == NULL && sock->state == SS_CONNECTING) {
    if (sock->closing)
      return -32; // EPIPE
    if (usk->listener == NULL || usk->orphaned)
      return -107; // ENOTCONN

    struct thread *current = sched_get_current();
    wait_queue_entry_t entry = {.thread = current, .next = NULL};

    wait_queue_add(usk->wait, &entry);
    current->state = THREAD_BLOCKED;

    if (usk->peer != NULL || sock->state != SS_CONNECTING) {
      current->state = THREAD_RUNNING;
    } else {
      sched_yield();
    }

    wait_queue_remove(usk->wait, &entry);
    current->state = THREAD_RUNNING;
  }

  unix_sock_t *peer = NULL;
  socket_t *peer_sock = unix_get_live_peer(sock, &peer);
  if (!peer_sock || !peer)
    return -107; // ENOTCONN

  size_t sent = 0;
  const uint8_t *src = (const uint8_t *)buf;

  while (sent < len) {
    if (sock->closing || peer_sock->closing)
      break;

    spinlock_acquire(&peer->recv_lock);

    if (!unix_ensure_recv_buf(peer)) {
      spinlock_release(&peer->recv_lock);
      if (sent > 0) break;
      socket_put(peer_sock);
      return -12; // ENOMEM
    }

    size_t head  = peer->recv_buf_head;
    size_t tail  = peer->recv_buf_tail;
    size_t size  = peer->recv_buf_size;
    size_t space = (size > 0) ? (head - tail - 1 + size) % size : 0;

    if (space == 0) {
      spinlock_release(&peer->recv_lock);

      if (sent > 0)
        break; // Return what we've sent so far

      if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) { // MSG_DONTWAIT
        socket_put(peer_sock);
        return -11; // EAGAIN
      }

      struct thread *current = sched_get_current();
      wait_queue_entry_t entry = {.thread = current, .next = NULL};

      wait_queue_add(peer->wait, &entry);
      current->state = THREAD_BLOCKED;

      size  = peer->recv_buf_size;
      space = (size > 0) ? (peer->recv_buf_head - peer->recv_buf_tail - 1 + size) % size : 0;

      if (space > 0) {
        current->state = THREAD_RUNNING;
      } else {
        sched_yield();
      }

      wait_queue_remove(peer->wait, &entry);
      current->state = THREAD_RUNNING;

      if (sock->closing || peer_sock->closing) {
        socket_put(peer_sock);
        return -32; // EPIPE
      }

      if (sock->error) {
        int err = sock->error;
        socket_put(peer_sock);
        return -err;
      }
      continue;
    }

    size_t to_copy = (len - sent < space) ? len - sent : space;

    for (size_t i = 0; i < to_copy; i++) {
      peer->recv_buf[tail] = src[sent + i];
      tail = (size > 0) ? (tail + 1) % size : 0;
    }
    peer->recv_buf_tail = tail;
    sent += to_copy;

    unix_record_sender_credentials(peer, sched_get_current());

    spinlock_release(&peer->recv_lock);

    wait_queue_wake_all(peer->wait);

    if (peer->parent && peer->parent->wait_queue)
      wait_queue_wake_all((wait_queue_t *)peer->parent->wait_queue);

    if (peer->parent && peer->parent->node)
      epoll_notify_event(peer->parent->node, EPOLLIN | EPOLLRDNORM);
    else if (peer->parent)
      epoll_notify_socket(peer->parent->fd, EPOLLIN);
  }

  bool closed = sock->closing || peer_sock->closing;
  socket_put(peer_sock);
  if (sent == 0 && closed)
    return -32; // EPIPE
  return (ssize_t)sent;
}

ssize_t unix_recv_impl(socket_t *sock, void *buf, size_t len, int flags) {
  if (!sock || !sock->sk)
    return -9; // EBADF

  unix_sock_t *usk = (unix_sock_t *)sock->sk;

  if (sock->closing && usk->recv_buf_head == usk->recv_buf_tail)
    return 0; // EOF

  if (sock->state != SS_CONNECTED &&
      usk->recv_buf_head == usk->recv_buf_tail) {
    return 0; // EOF
  }

  uint8_t *dest     = (uint8_t *)buf;
  size_t  received  = 0;

  while (received < len) {
    spinlock_acquire(&usk->recv_lock);

    size_t head      = usk->recv_buf_head;
    size_t tail      = usk->recv_buf_tail;
    size_t size      = usk->recv_buf_size;
    size_t available = (size > 0) ? (tail - head + size) % size : 0;

    if (available == 0) {
      spinlock_release(&usk->recv_lock);

      if (received > 0)
        break;

      if (sock->closing)
        return 0; // EOF

      if (sock->state != SS_CONNECTED) {
        usk->accepted_orphaned = false;
        return 0; // EOF
      }

      if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) // MSG_DONTWAIT
        return -11; // EAGAIN

      struct thread *current = sched_get_current();
      wait_queue_entry_t entry = {.thread = current, .next = NULL};

      wait_queue_add(usk->wait, &entry);
      current->state = THREAD_BLOCKED;

      head      = usk->recv_buf_head;
      tail      = usk->recv_buf_tail;
      size      = usk->recv_buf_size;
      available = (size > 0) ? (tail - head + size) % size : 0;

      if (available > 0 || sock->state != SS_CONNECTED) {
        current->state = THREAD_RUNNING;
      } else {
        sched_yield();
      }

      wait_queue_remove(usk->wait, &entry);
      current->state = THREAD_RUNNING;

      if (sock->closing)
        return 0; // EOF

      if (sock->error)
        return -sock->error;
      continue;
    }

    size_t to_copy = (len - received < available) ? len - received : available;

    for (size_t i = 0; i < to_copy; i++)
      dest[received + i] = (size > 0) ? usk->recv_buf[(head + i) % size] : 0;

    if (!(flags & 0x02)) { // MSG_PEEK
      usk->recv_buf_head = (size > 0) ? (head + to_copy) % size : 0;
      usk->scm_cred_pending = false;
    }

    spinlock_release(&usk->recv_lock);
    received += to_copy;

    if (sock->type == SOCK_DGRAM)
      break;
  }

  wait_queue_wake_all(usk->wait);

  unix_sock_t *notify_peer = NULL;
  socket_t *notify_peer_sock = unix_get_live_peer(sock, &notify_peer);
  if (notify_peer_sock) {
    if (notify_peer_sock->node)
      epoll_notify_event(notify_peer_sock->node, EPOLLOUT | EPOLLWRNORM);
    socket_put(notify_peer_sock);
  }

  return (ssize_t)received;
}

ssize_t unix_sendto_impl(socket_t *sock, const void *buf, size_t len,
                         int flags, struct sockaddr *dest_addr, int addrlen) {
  (void)addrlen;
  if (!dest_addr)
    return unix_send_impl(sock, buf, len, flags);

  klog_puts("[WARN] unix_sendto: with address not implemented (DGRAM)\n");
  return -95; // EOPNOTSUPP
}

ssize_t unix_recvfrom_impl(socket_t *sock, void *buf, size_t len, int flags,
                           struct sockaddr *src_addr, int *addrlen) {
  ssize_t ret = unix_recv_impl(sock, buf, len, flags);
  if (ret >= 0 && src_addr && addrlen) {
    unix_sock_t *peer = NULL;
    socket_t *peer_sock = unix_get_live_peer(sock, &peer);
    if (peer_sock && peer) {
      int to_copy = peer->addr_len < *addrlen ? peer->addr_len : *addrlen;
      memcpy(src_addr, &peer->addr, to_copy);
      *addrlen = to_copy;
      socket_put(peer_sock);
    } else {
      *addrlen = 0;
    }
  }
  return ret;
}

ssize_t unix_sendmsg_impl(socket_t *sock, struct msghdr *msg, int flags) {
  if (!sock || !msg)
    return -22; // EINVAL
  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;
  if (sock->closing)
    return -32; // EPIPE

  unix_sock_t *peer = NULL;
  socket_t *peer_sock = unix_get_live_peer(sock, &peer);
  if (!peer_sock || !peer)
    return -107; // ENOTCONN

  struct thread *current = sched_get_current();

  size_t total_len = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++)
    total_len += msg->msg_iov[i].iov_len;

  // Hold peer->recv_lock for the entire sendmsg so SCM delivery is atomic.
  spinlock_acquire(&peer->recv_lock);

  if (!unix_ensure_recv_buf(peer)) {
    spinlock_release(&peer->recv_lock);
    socket_put(peer_sock);
    return -12; // ENOMEM
  }

  while (total_len > 0) {
    size_t head  = peer->recv_buf_head;
    size_t tail  = peer->recv_buf_tail;
    size_t size  = peer->recv_buf_size;
    size_t space = (size > 0) ? (head - tail - 1 + size) % size : 0;

    if (space >= total_len)
      break;

    if (space == 0) {
      spinlock_release(&peer->recv_lock);

      if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40)) {
        socket_put(peer_sock);
        return -11; // EAGAIN
      }

      struct thread *ct = sched_get_current();
      wait_queue_entry_t entry = {.thread = ct, .next = NULL};
      wait_queue_add(peer->wait, &entry);
      ct->state = THREAD_BLOCKED;
      sched_yield();
      wait_queue_remove(peer->wait, &entry);
      ct->state = THREAD_RUNNING;

      if (sock->closing || peer_sock->closing) {
        socket_put(peer_sock);
        return -32; // EPIPE
      }

      spinlock_acquire(&peer->recv_lock);
      continue;
    }
    break; // Partial space – write what fits
  }

  // Deliver SCM_RIGHTS nodes atomically (lock order: recv_lock → parent->lock)
  spinlock_acquire(&peer_sock->lock);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
  while (cmsg) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      int *fds  = (int *)CMSG_DATA(cmsg);
      int count = (int)((cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) /
                        sizeof(int));

      for (int i = 0; i < count && peer->scm_count < 16; i++) {
        int fd = fds[i];
        if (fd >= 0 && fd < MAX_FDS && current->fds[fd]) {
          vfs_node_t *node = current->fds[fd];
          vfs_open(node);
          peer->scm_nodes[peer->scm_count++] = node;
        }
      }
    }
    cmsg = CMSG_NXTHDR(msg, cmsg);
  }

  spinlock_release(&peer_sock->lock);

  // Write iovec data into peer's ring buffer
  ssize_t total_sent = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    const uint8_t *src = (const uint8_t *)msg->msg_iov[i].iov_base;
    size_t len  = msg->msg_iov[i].iov_len;
    size_t sent = 0;

    while (sent < len) {
      size_t head  = peer->recv_buf_head;
      size_t tail  = peer->recv_buf_tail;
      size_t size  = peer->recv_buf_size;
      size_t space = (size > 0) ? (head - tail - 1 + size) % size : 0;

      if (space == 0)
        break;

      size_t to_copy = (len - sent < space) ? len - sent : space;
      for (size_t j = 0; j < to_copy; j++) {
        peer->recv_buf[tail] = src[sent + j];
        tail = (size > 0) ? (tail + 1) % size : 0;
      }
      peer->recv_buf_tail = tail;
      sent += to_copy;
    }
    total_sent += (ssize_t)sent;
  }

  if (total_sent > 0)
    unix_record_sender_credentials(peer, current);

  spinlock_release(&peer->recv_lock);

  // Wake receiver
  wait_queue_wake_all(peer->wait);
  if (peer->parent && peer->parent->wait_queue)
    wait_queue_wake_all((wait_queue_t *)peer->parent->wait_queue);

  if (peer->parent && peer->parent->node)
    epoll_notify_event(peer->parent->node, EPOLLIN | EPOLLRDNORM);
  else if (peer->parent)
    epoll_notify_socket(peer->parent->fd, EPOLLIN);

  socket_put(peer_sock);
  return total_sent;
}

ssize_t unix_recvmsg_impl(socket_t *sock, struct msghdr *msg, int flags) {
  if (!sock || !msg)
    return -22; // EINVAL
  unix_sock_t *usk = (unix_sock_t *)sock->sk;
  if (!usk)
    return -22;
  if (sock->closing)
    goto no_data;

  struct thread *current = sched_get_current();

  // Block until data is available; keep recv_lock held on exit from loop
  while (1) {
    spinlock_acquire(&usk->recv_lock);

    size_t head      = usk->recv_buf_head;
    size_t tail      = usk->recv_buf_tail;
    size_t size      = usk->recv_buf_size;
    size_t available = (size > 0) ? (tail - head + size) % size : 0;

    if (available > 0)
      break; // Data ready – recv_lock stays held

    spinlock_release(&usk->recv_lock);

    if (sock->closing || sock->state != SS_CONNECTED)
      goto no_data;

    if ((sock->flags & SOCK_NONBLOCK) || (flags & 0x40))
      return -11; // EAGAIN

    uint64_t rcvtimeo_deadline = 0;
    if (usk->rcvtimeo_ms > 0) {
      rcvtimeo_deadline = lapic_timer_get_ticks() + (uint64_t)usk->rcvtimeo_ms;
    }

    struct thread *ct = sched_get_current();
    wait_queue_entry_t entry = {.thread = ct, .next = NULL};
    wait_queue_add(usk->wait, &entry);
    ct->state = THREAD_BLOCKED;
    if (rcvtimeo_deadline != 0) {
      ct->wakeup_ticks = rcvtimeo_deadline;
    }
    sched_yield();
    wait_queue_remove(usk->wait, &entry);
    ct->state = THREAD_RUNNING;
    ct->wakeup_ticks = 0;

    if (rcvtimeo_deadline != 0 && lapic_timer_get_ticks() >= rcvtimeo_deadline) {
      return -11; // EAGAIN
    }

    if (sock->closing)
      goto no_data;

    if (sock->error)
      return -(int)sock->error;
  }

  // recv_lock held; also acquire sock->lock to dequeue SCM nodes atomically.
  // Lock order: recv_lock → parent->lock  (same as sendmsg)
  spinlock_acquire(&sock->lock);

  size_t control_capacity = msg->msg_controllen;
  size_t control_used = 0;
  bool have_control = msg->msg_control && control_capacity > 0;

  if (have_control &&
      !unix_user_range_valid((uint64_t)(uintptr_t)msg->msg_control,
                             control_capacity)) {
    spinlock_release(&sock->lock);
    spinlock_release(&usk->recv_lock);
    return -14; // EFAULT
  }

  if (usk->scm_cred_pending) {
    size_t needed = CMSG_SPACE(sizeof(struct unix_ucred));
    if (have_control && control_capacity - control_used >= needed) {
      struct cmsghdr *cmsg = (struct cmsghdr *)
          ((uint8_t *)msg->msg_control + control_used);
      cmsg->cmsg_len = CMSG_LEN(sizeof(struct unix_ucred));
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_CREDENTIALS;

      struct unix_ucred *cred = (struct unix_ucred *)CMSG_DATA(cmsg);
      cred->pid = usk->scm_cred_pid;
      cred->uid = usk->scm_cred_uid;
      cred->gid = usk->scm_cred_gid;
      control_used += needed;

      if (!(flags & 0x02)) // MSG_PEEK
        usk->scm_cred_pending = false;
    } else {
      msg->msg_flags |= MSG_CTRUNC;
      if (!(flags & 0x02)) // MSG_PEEK
        usk->scm_cred_pending = false;
    }
  }

  if (usk->scm_count > 0 && have_control &&
      control_capacity - control_used >= CMSG_SPACE(sizeof(int))) {
    struct cmsghdr *cmsg = (struct cmsghdr *)
        ((uint8_t *)msg->msg_control + control_used);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;

    int *fds = (int *)CMSG_DATA(cmsg);
    int max_fds = (int)((control_capacity - control_used -
                         CMSG_ALIGN(sizeof(struct cmsghdr))) / sizeof(int));
    if (max_fds > usk->scm_count)
      max_fds = usk->scm_count;

    int actual_count = 0;
    for (int i = 0; i < max_fds; i++) {
      vfs_node_t *node = usk->scm_nodes[i];
      int new_fd = alloc_fd(current);
      if (new_fd >= 0) {
        current->fds[new_fd] = node;
        fds[actual_count++] = new_fd;
      } else {
        vfs_close(node);
      }
      usk->scm_nodes[i] = NULL;
    }

    // Shift remaining nodes down
    int remaining = usk->scm_count - max_fds;
    for (int i = 0; i < remaining; i++)
      usk->scm_nodes[i] = usk->scm_nodes[max_fds + i];
    usk->scm_count = remaining;

    cmsg->cmsg_len = CMSG_LEN(actual_count * sizeof(int));
    control_used += CMSG_SPACE(actual_count * sizeof(int));

    if (remaining > 0)
      msg->msg_flags |= MSG_CTRUNC;
  } else if (usk->scm_count > 0) {
    msg->msg_flags |= MSG_CTRUNC;
  }

  msg->msg_controllen = control_used;

  spinlock_release(&sock->lock);

  // Read data from ring buffer (recv_lock still held)
  ssize_t total_received = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    uint8_t *dest = (uint8_t *)msg->msg_iov[i].iov_base;
    size_t   want = msg->msg_iov[i].iov_len;

    size_t head      = usk->recv_buf_head;
    size_t tail      = usk->recv_buf_tail;
    size_t size      = usk->recv_buf_size;
    size_t available = (size > 0) ? (tail - head + size) % size : 0;

    if (available == 0)
      break;

    size_t to_copy = (want < available) ? want : available;
    for (size_t j = 0; j < to_copy; j++)
      dest[j] = (size > 0) ? usk->recv_buf[(head + j) % size] : 0;

    if (!(flags & 0x02)) // MSG_PEEK
      usk->recv_buf_head = (size > 0) ? (head + to_copy) % size : 0;

    total_received += (ssize_t)to_copy;
  }

  spinlock_release(&usk->recv_lock);

  wait_queue_wake_all(usk->wait);

  unix_sock_t *notify_peer = NULL;
  socket_t *notify_peer_sock = unix_get_live_peer(sock, &notify_peer);
  if (notify_peer_sock) {
    if (notify_peer_sock->node)
      epoll_notify_event(notify_peer_sock->node, EPOLLOUT | EPOLLWRNORM);
    socket_put(notify_peer_sock);
  }

  msg->msg_flags &= ~MSG_TRUNC;
  return total_received;

no_data:
  msg->msg_controllen = 0;
  msg->msg_flags      = 0;
  return 0;
}
