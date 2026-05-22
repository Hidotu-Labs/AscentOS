// ── Socket Syscalls: socket, socketpair, bind, connect, listen, accept, etc.
// ──── Phase 1: Basic socket creation and FD management

#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../socket/af_inet.h"
#include "../socket/socket.h"
#include "../socket/socket_internal.h"
#include "syscall.h"
#include <stdint.h>

// User-space pointer validation
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL
static inline bool is_user_ptr(uint64_t addr) {
  return addr != 0 && addr <= USER_ADDR_MAX;
}

// ── Syscall: socket(int domain, int type, int protocol)
// ─────────────────────── Returns: file descriptor or negative error
static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                           uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int dom = (int)domain;
  int typ = (int)type;
  int proto = (int)protocol;

  // Create socket
  socket_t *sock = socket_create(dom, typ, proto);
  if (!sock) {
    // Determine error
    if (dom != AF_UNIX)
      return (uint64_t)-EAFNOSUPPORT;
    if (proto != 0)
      return (uint64_t)-EPROTONOSUPPORT;
    if (typ != SOCK_STREAM && typ != SOCK_DGRAM)
      return (uint64_t)-EPROTONOSUPPORT;
    return (uint64_t)-12; // ENOMEM
  }

  // Allocate FD
  int fd = socket_alloc_fd(sock);
  if (fd < 0) {
    socket_put(sock);
    return (uint64_t)fd;
  }

  return (uint64_t)fd;
}

// ── Syscall: socketpair(int domain, int type, int protocol, int sv[2])
// ──────── Returns: 0 on success, negative error on failure
static uint64_t sys_socketpair(uint64_t domain, uint64_t type,
                               uint64_t protocol, uint64_t sv_ptr,
                               uint64_t _arg4, uint64_t _arg5) {
  (void)_arg4;
  (void)_arg5;

  int dom = (int)domain;
  int typ = (int)type;
  int proto = (int)protocol;

  // Validate sv pointer
  if (!is_user_ptr(sv_ptr) || (sv_ptr & 3)) {
    return (uint64_t)-14; // EFAULT
  }

  // Create socket pair
  socket_t *socks[2] = {NULL, NULL};
  int ret = socket_create_pair(dom, typ, proto, socks);
  if (ret < 0) {
    return (uint64_t)ret;
  }

  // Allocate FDs
  int fd0 = socket_alloc_fd(socks[0]);
  if (fd0 < 0) {
    socket_put(socks[0]);
    socket_put(socks[1]);
    return (uint64_t)fd0;
  }

  int fd1 = socket_alloc_fd(socks[1]);
  if (fd1 < 0) {
    socket_close_fd(fd0);
    socket_put(socks[1]);
    return (uint64_t)fd1;
  }

  // Write FDs to user space
  int *sv = (int *)sv_ptr;
  sv[0] = fd0;
  sv[1] = fd1;

  return 0;
}

// ── Syscall: bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
// ─
static uint64_t sys_bind(uint64_t sockfd, uint64_t addr_ptr, uint64_t addrlen,
                         uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;
  struct sockaddr *addr = (struct sockaddr *)addr_ptr;

  // Validate address pointer
  if (!is_user_ptr(addr_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  int ret = socket_bind(sock, addr, (int)addrlen);
  return (uint64_t)ret;
}

// ── Syscall: connect(int sockfd, const struct sockaddr *addr, socklen_t
// addrlen)
static uint64_t sys_connect(uint64_t sockfd, uint64_t addr_ptr,
                            uint64_t addrlen, uint64_t _arg3, uint64_t _arg4,
                            uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;
  struct sockaddr *addr = (struct sockaddr *)addr_ptr;

  klog_puts("[CONNECT] fd=");
  klog_uint64(fd);
  klog_puts(" addr_ptr=");
  klog_uint64(addr_ptr);
  klog_puts(" addrlen=");
  klog_uint64(addrlen);
  klog_puts("\n");

  // Validate address pointer
  if (!is_user_ptr(addr_ptr)) {
    klog_puts("[CONNECT] EFAULT: invalid addr_ptr\n");
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    klog_puts("[CONNECT] EBADF: invalid fd\n");
    return (uint64_t)-9; // EBADF
  }

  klog_puts("[CONNECT] family=");
  klog_uint64(addr->sa_family);
  klog_puts(" domain=");
  klog_uint64(sock->domain);
  klog_puts(" path=");
  // For AF_UNIX, print the path
  if (addr->sa_family == 1 && addrlen > 2) {
    struct sockaddr_un *sun = (struct sockaddr_un *)addr;
    if (sun->sun_path[0] == '\0') {
      // Abstract socket - print @ followed by the name
      klog_puts("@");
      klog_puts(sun->sun_path + 1);
    } else {
      klog_puts(sun->sun_path);
    }
  }
  klog_puts("\n");

  int ret = socket_connect(sock, addr, (int)addrlen);
  klog_puts("[CONNECT] returned ");
  klog_uint64((uint64_t)ret);
  klog_puts("\n");
  return (uint64_t)ret;
}

// ── Syscall: listen(int sockfd, int backlog)
// ──────────────────────────────────
static uint64_t sys_listen(uint64_t sockfd, uint64_t backlog, uint64_t _arg2,
                           uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg2;
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  int ret = socket_listen(sock, (int)backlog);
  return (uint64_t)ret;
}

// ── Syscall: accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
// ────
static uint64_t sys_accept(uint64_t sockfd, uint64_t addr_ptr,
                           uint64_t addrlen_ptr, uint64_t _arg3, uint64_t _arg4,
                           uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  // Validate pointers (can be NULL)
  struct sockaddr *addr = NULL;
  int *addrlen = NULL;
  if (addr_ptr && is_user_ptr(addr_ptr)) {
    addr = (struct sockaddr *)addr_ptr;
  }
  if (addrlen_ptr && is_user_ptr(addrlen_ptr)) {
    addrlen = (int *)addrlen_ptr;
  }

  // Accept connection
  socket_t *newsock = NULL;
  int ret = socket_accept(sock, &newsock);
  if (ret < 0) {
    return (uint64_t)ret;
  }

  // Allocate FD for new socket
  int newfd = socket_alloc_fd(newsock);
  if (newfd < 0) {
    socket_put(newsock);
    return (uint64_t)newfd;
  }

  // Fill in addr and addrlen for AF_INET
  if (newsock->domain == AF_INET) {
    inet_sock_t *newinet = (inet_sock_t *)newsock->sk;
    if (addr && addrlen) {
      struct sockaddr_in sin;
      memset(&sin, 0, sizeof(sin));
      sin.sin_family = AF_INET;
      sin.sin_port = newinet->remote_addr.sin_port;
      sin.sin_addr.s_addr = newinet->remote_addr.sin_addr.s_addr;

      int copy_len = *addrlen < (int)sizeof(sin) ? *addrlen : (int)sizeof(sin);
      memcpy(addr, &sin, copy_len);
      *addrlen = sizeof(sin);
    }
  }

  return (uint64_t)newfd;
}

// ── Syscall: sendto(int sockfd, const void *buf, size_t len, int flags, ...)
// ──
static uint64_t sys_sendto(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                           uint64_t flags, uint64_t dest_addr_ptr,
                           uint64_t addrlen) {
  int fd = (int)sockfd;

  // Validate buffer pointer
  if (!is_user_ptr(buf_ptr) || (len > 0 && !is_user_ptr(buf_ptr + len - 1))) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  const void *buf = (const void *)buf_ptr;
  struct sockaddr *dest_addr = NULL;
  if (dest_addr_ptr) {
    if (!is_user_ptr(dest_addr_ptr))
      return (uint64_t)-14;
    dest_addr = (struct sockaddr *)dest_addr_ptr;
  }

  ssize_t ret =
      socket_sendto(sock, buf, len, (int)flags, dest_addr, (int)addrlen);
  return (uint64_t)ret;
}

// ── Syscall: recvfrom(int sockfd, void *buf, size_t len, int flags, ...) ─────
static uint64_t sys_recvfrom(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                             uint64_t flags, uint64_t src_addr_ptr,
                             uint64_t addrlen_ptr) {
  int fd = (int)sockfd;

  // Validate buffer pointer
  if (!is_user_ptr(buf_ptr) || (len > 0 && !is_user_ptr(buf_ptr + len - 1))) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  void *buf = (void *)buf_ptr;
  struct sockaddr *src_addr = NULL;
  int *addrlen = NULL;

  if (src_addr_ptr) {
    if (!is_user_ptr(src_addr_ptr))
      return (uint64_t)-14;
    src_addr = (struct sockaddr *)src_addr_ptr;
  }

  if (addrlen_ptr) {
    if (!is_user_ptr(addrlen_ptr))
      return (uint64_t)-14;
    addrlen = (int *)addrlen_ptr;
  }

  ssize_t ret = socket_recvfrom(sock, buf, len, (int)flags, src_addr, addrlen);
  return (uint64_t)ret;
}

// ── Structures for recvmsg/sendmsg ─────────────────────────────────────────
struct iovec {
  void *iov_base;
  size_t iov_len;
};

struct msghdr {
  void *msg_name;        // Source address (for recvmsg)
  size_t msg_namelen;    // Address length
  struct iovec *msg_iov; // Scatter/gather array
  size_t msg_iovlen;     // Number of iovec elements
  void *msg_control;     // Ancillary data
  size_t msg_controllen; // Ancillary data length
  int msg_flags;         // Flags on received message
};

// ── Syscall: sendmsg(int sockfd, struct msghdr *msg, int flags)
// ───────────────────────────────────────────────────────────────────
static uint64_t sys_sendmsg(uint64_t sockfd, uint64_t msg_ptr, uint64_t flags,
                            uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Validate msghdr pointer
  if (!is_user_ptr(msg_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  struct msghdr *msg = (struct msghdr *)msg_ptr;

  // Validate iovec array
  if (!is_user_ptr((uint64_t)msg->msg_iov)) {
    return (uint64_t)-14; // EFAULT
  }

  // Send data from iovec buffers
  ssize_t total_sent = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    struct iovec *iov = &msg->msg_iov[i];

    if (!is_user_ptr((uint64_t)iov->iov_base)) {
      return (uint64_t)-14; // EFAULT
    }

    if (iov->iov_len == 0) {
      continue;
    }

    ssize_t ret = socket_send(sock, iov->iov_base, iov->iov_len, (int)flags);
    if (ret < 0) {
      if (total_sent > 0) {
        return (uint64_t)total_sent;
      }
      return (uint64_t)ret;
    }

    total_sent += ret;

    // If we sent less than requested, we're done (buffer full)
    if ((size_t)ret < iov->iov_len) {
      break;
    }
  }

  return (uint64_t)total_sent;
}

// ── Syscall: recvmsg(int sockfd, struct msghdr *msg, int flags)
// ───────────────────────────────────────────────────────────────────
static uint64_t sys_recvmsg(uint64_t sockfd, uint64_t msg_ptr, uint64_t flags,
                            uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Validate msghdr pointer
  if (!is_user_ptr(msg_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  struct msghdr *msg = (struct msghdr *)msg_ptr;

  // Validate iovec array
  if (!is_user_ptr((uint64_t)msg->msg_iov)) {
    return (uint64_t)-14; // EFAULT
  }

  // Receive data into iovec buffers
  ssize_t total_received = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    struct iovec *iov = &msg->msg_iov[i];

    if (!is_user_ptr((uint64_t)iov->iov_base)) {
      return (uint64_t)-14; // EFAULT
    }

    if (iov->iov_len == 0) {
      continue;
    }

    ssize_t ret = socket_recv(sock, iov->iov_base, iov->iov_len, (int)flags);
    if (ret < 0) {
      if (total_received > 0) {
        return (uint64_t)total_received;
      }
      return (uint64_t)ret;
    }

    total_received += ret;

    // If we received less than requested, we're done
    if ((size_t)ret < iov->iov_len) {
      break;
    }
  }

  // Update msghdr fields for user space
  msg->msg_namelen = 0;
  msg->msg_controllen = 0;
  msg->msg_flags = 0;

  return (uint64_t)total_received;
}

// ── Syscall: shutdown(int sockfd, int how)
// ────────────────────────────────────
static uint64_t sys_shutdown(uint64_t sockfd, uint64_t how, uint64_t _arg2,
                             uint64_t _arg3, uint64_t _arg4, uint64_t _arg5) {
  (void)_arg2;
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  if (!sock->ops || !sock->ops->shutdown) {
    return (uint64_t)-95; // EOPNOTSUPP
  }

  int ret = sock->ops->shutdown(sock, (int)how);
  return (uint64_t)ret;
}

// ── Syscall: getsockopt(int sockfd, int level, int optname, ...)
// ──────────────
static uint64_t sys_getsockopt(uint64_t sockfd, uint64_t level,
                               uint64_t optname, uint64_t optval_ptr,
                               uint64_t optlen_ptr, uint64_t _arg5) {
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  // Validate pointers
  if (!is_user_ptr(optval_ptr) || !is_user_ptr(optlen_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  void *optval = (void *)optval_ptr;
  int *optlen = (int *)optlen_ptr;

  // Validate optlen
  if (*optlen < (int)sizeof(int)) {
    return (uint64_t)-22; // EINVAL
  }

  // Handle socket-level options
  if ((int)level == SOL_SOCKET) {
    int *val = (int *)optval;
    switch ((int)optname) {
    case SO_TYPE:
      *val = sock->type;
      *optlen = sizeof(int);
      return 0;
    case SO_DOMAIN:
      *val = sock->domain;
      *optlen = sizeof(int);
      return 0;
    case SO_PROTOCOL:
      *val = sock->protocol;
      *optlen = sizeof(int);
      return 0;
    case SO_ERROR:
      *val = socket_get_error(sock);
      *optlen = sizeof(int);
      return 0;
    case SO_RCVBUF:
      *val = sock->rcvbuf;
      *optlen = sizeof(int);
      return 0;
    case SO_SNDBUF:
      *val = sock->sndbuf;
      *optlen = sizeof(int);
      return 0;
    case SO_REUSEADDR:
      *val = sock->reuseaddr;
      *optlen = sizeof(int);
      return 0;
    default:
      break;
    }
  }

  // Try family-specific handler
  if (sock->ops && sock->ops->getsockopt) {
    int ret =
        sock->ops->getsockopt(sock, (int)level, (int)optname, optval, optlen);
    return (uint64_t)ret;
  }

  return (uint64_t)-92; // ENOPROTOOPT
}

// ── Syscall: setsockopt(int sockfd, int level, int optname, ...)
// ──────────────
static uint64_t sys_setsockopt(uint64_t sockfd, uint64_t level,
                               uint64_t optname, uint64_t optval_ptr,
                               uint64_t optlen, uint64_t _arg5) {
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  // Validate pointer
  if (!is_user_ptr(optval_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  const void *optval = (const void *)optval_ptr;

  // Validate optlen
  if (optlen < sizeof(int)) {
    return (uint64_t)-22; // EINVAL
  }

  // Handle socket-level options
  if ((int)level == SOL_SOCKET) {
    const int *val = (const int *)optval;
    switch ((int)optname) {
    case SO_RCVBUF: {
      int v = *val;
      if (v < 256)
        v = 256;
      if (v > 1024 * 1024)
        v = 1024 * 1024; // 1MB max
      sock->rcvbuf = v;
      return 0;
    }
    case SO_SNDBUF: {
      int v = *val;
      if (v < 256)
        v = 256;
      if (v > 1024 * 1024)
        v = 1024 * 1024; // 1MB max
      sock->sndbuf = v;
      return 0;
    }
    case SO_REUSEADDR:
      sock->reuseaddr = *val;
      return 0;
    default:
      break;
    }
  }

  // Try family-specific handler
  if (sock->ops && sock->ops->setsockopt) {
    int ret = sock->ops->setsockopt(sock, (int)level, (int)optname, optval,
                                    (int)optlen);
    return (uint64_t)ret;
  }

  return (uint64_t)-92; // ENOPROTOOPT
}

// ── Syscall: getsockname(int sockfd, struct sockaddr *addr, ...)
// ──────────────
static uint64_t sys_getsockname(uint64_t sockfd, uint64_t addr_ptr,
                                uint64_t addrlen_ptr, uint64_t _arg3,
                                uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  // Validate user pointers
  if (!is_user_ptr(addr_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  if (!is_user_ptr(addrlen_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket family-specific data
  if (sock->domain == AF_UNIX) {
    unix_sock_t *usock = (unix_sock_t *)sock->sk;
    if (!usock) {
      return (uint64_t)-22; // EINVAL
    }

    int *addrlen = (int *)addrlen_ptr;
    int user_len = *addrlen;

    // Calculate actual address length
    int actual_len = usock->addr_len;
    if (actual_len == 0) {
      // Socket not bound, return empty address
      actual_len = sizeof(sa_family_t);
    }

    // Copy as much as fits
    int copy_len = user_len < actual_len ? user_len : actual_len;

    // Copy address to user space
    if (copy_len > 0) {
      memcpy((void *)addr_ptr, &usock->addr, copy_len);
    }

    // Update addrlen to actual size
    *addrlen = actual_len;

    return 0;
  } else if (sock->domain == AF_INET) {
    inet_sock_t *inet = (inet_sock_t *)sock->sk;
    if (!inet)
      return (uint64_t)-22;

    int *addrlen = (int *)addrlen_ptr;
    int user_len = *addrlen;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = inet->local_addr.sin_port;
    sin.sin_addr.s_addr = inet->local_addr.sin_addr.s_addr;

    int actual_len = sizeof(struct sockaddr_in);
    int copy_len = user_len < actual_len ? user_len : actual_len;

    if (copy_len > 0) {
      memcpy((void *)addr_ptr, &sin, copy_len);
    }
    *addrlen = actual_len;

    return 0;
  }

  return (uint64_t)-95; // EOPNOTSUPP
}

// ── Syscall: getpeername(int sockfd, struct sockaddr *addr, ...)
// ──────────────
static uint64_t sys_getpeername(uint64_t sockfd, uint64_t addr_ptr,
                                uint64_t addrlen_ptr, uint64_t _arg3,
                                uint64_t _arg4, uint64_t _arg5) {
  (void)_arg3;
  (void)_arg4;
  (void)_arg5;

  int fd = (int)sockfd;

  // Get socket from FD
  socket_t *sock = socket_from_fd(fd);
  if (!sock) {
    return (uint64_t)-9; // EBADF
  }

  // Validate user pointers
  if (!is_user_ptr(addr_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  if (!is_user_ptr(addrlen_ptr)) {
    return (uint64_t)-14; // EFAULT
  }

  // Get socket family-specific data
  if (sock->domain == AF_UNIX) {
    unix_sock_t *usock = (unix_sock_t *)sock->sk;
    if (!usock) {
      return (uint64_t)-22; // EINVAL
    }

    // Socket must be connected
    if (sock->state != SS_CONNECTED) {
      return (uint64_t)-107; // ENOTCONN
    }

    int *addrlen = (int *)addrlen_ptr;
    int user_len = *addrlen;

    // Get peer's address
    unix_sock_t *peer = usock->peer;
    if (!peer) {
      return (uint64_t)-107; // ENOTCONN
    }

    // Calculate actual address length
    int actual_len = peer->addr_len;
    if (actual_len == 0) {
      // Peer not bound, return empty address
      actual_len = sizeof(sa_family_t);
    }

    // Copy as much as fits
    int copy_len = user_len < actual_len ? user_len : actual_len;

    // Copy peer address to user space
    if (copy_len > 0) {
      memcpy((void *)addr_ptr, &peer->addr, copy_len);
    }

    // Update addrlen to actual size
    *addrlen = actual_len;

    return 0;
  } else if (sock->domain == AF_INET) {
    inet_sock_t *inet = (inet_sock_t *)sock->sk;
    if (!inet)
      return (uint64_t)-22;

    if (sock->state != SS_CONNECTED)
      return (uint64_t)-107; // ENOTCONN

    int *addrlen = (int *)addrlen_ptr;
    int user_len = *addrlen;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = inet->remote_addr.sin_port;
    sin.sin_addr.s_addr = inet->remote_addr.sin_addr.s_addr;

    int actual_len = sizeof(struct sockaddr_in);
    int copy_len = user_len < actual_len ? user_len : actual_len;

    if (copy_len > 0) {
      memcpy((void *)addr_ptr, &sin, copy_len);
    }
    *addrlen = actual_len;
    return 0;
  }

  return (uint64_t)-95; // EOPNOTSUPP
}

// ── Socket Syscall Registration
// ───────────────────────────────────────────────
void syscall_register_socket(void) {
  syscall_register(SYS_SOCKET, sys_socket);
  syscall_register(SYS_SOCKETPAIR, sys_socketpair);
  syscall_register(SYS_BIND, sys_bind);
  syscall_register(SYS_CONNECT, sys_connect);
  syscall_register(SYS_LISTEN, sys_listen);
  syscall_register(SYS_ACCEPT, sys_accept);
  syscall_register(SYS_SENDTO, sys_sendto);
  syscall_register(SYS_RECVFROM, sys_recvfrom);
  syscall_register(SYS_RECVMSG, sys_recvmsg);
  syscall_register(SYS_SHUTDOWN, sys_shutdown);
  syscall_register(SYS_SETSOCKOPT, sys_setsockopt);
  syscall_register(SYS_GETSOCKOPT, sys_getsockopt);
  syscall_register(SYS_SENDMSG, sys_sendmsg);
  syscall_register(SYS_GETSOCKNAME, sys_getsockname);
  syscall_register(SYS_GETPEERNAME, sys_getpeername);

  klog_puts("[OK] Socket syscalls registered\n");
}
