#ifndef AF_INET_H
#define AF_INET_H

#include "socket_internal.h"

// ── AF_INET Socket Initialization ───────────────────────────────────────────
void af_inet_init(void);
void af_inet_self_test(void);

// ── AF_INET Internal Structure ──────────────────────────────────────────────
typedef struct inet_sock {
  socket_t *parent;
  struct sockaddr_in local_addr;
  struct sockaddr_in remote_addr;

  // TCP socket ID from the raw TCP stack (-1 = not connected)
  int tcp_sock_id;
  uint16_t udp_port;

  // Protocol specific data (TCP/UDP)
  void *proto_data;

  // Receive queue
  sk_buff_head_t receive_queue;
} inet_sock_t;

// ── AF_INET Operations ──────────────────────────────────────────────────────
int inet_create(socket_t *sock, int protocol);
int inet_bind(socket_t *sock, struct sockaddr *addr, int addrlen);
int inet_connect(socket_t *sock, struct sockaddr *addr, int addrlen);
int inet_listen(socket_t *sock, int backlog);
int inet_accept(socket_t *sock, socket_t **newsock);
int inet_poll(socket_t *sock, int events);
ssize_t inet_send(socket_t *sock, const void *buf, size_t len, int flags);
ssize_t inet_recv(socket_t *sock, void *buf, size_t len, int flags);
ssize_t inet_sendto(socket_t *sock, const void *buf, size_t len, int flags,
                    struct sockaddr *dest_addr, int addrlen);
ssize_t inet_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                      struct sockaddr *src_addr, int *addrlen);
int inet_getsockname(socket_t *sock, struct sockaddr *addr, int *addrlen);
int inet_getpeername(socket_t *sock, struct sockaddr *addr, int *addrlen);
void inet_destroy(socket_t *sock);

#endif // AF_INET_H
