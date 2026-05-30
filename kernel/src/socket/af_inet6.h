#ifndef AF_INET6_H
#define AF_INET6_H

#include "socket_internal.h"
#include "../net/ipv6.h"

/* ── inet6_sock: per-socket IPv6 state ───────────────────────────────────── */
typedef struct inet6_sock {
    socket_t           *parent;
    struct sockaddr_in6 local_addr;
    struct sockaddr_in6 remote_addr;

    int      tcp_sock_id;   /* -1 if not a TCP connection */
    uint16_t udp_port;      /* bound UDP port, 0 if unbound */

    sk_buff_head_t receive_queue;
} inet6_sock_t;

/* ── Initialise AF_INET6 family ──────────────────────────────────────────── */
void af_inet6_init(void);

/* ── Operations (called from sock_ops_t) ─────────────────────────────────── */
int     inet6_create(socket_t *sock, int protocol);
int     inet6_bind(socket_t *sock, struct sockaddr *addr, int addrlen);
int     inet6_connect(socket_t *sock, struct sockaddr *addr, int addrlen);
int     inet6_listen(socket_t *sock, int backlog);
int     inet6_accept(socket_t *sock, socket_t **newsock);
int     inet6_poll(socket_t *sock, int events);
ssize_t inet6_send(socket_t *sock, const void *buf, size_t len, int flags);
ssize_t inet6_recv(socket_t *sock, void *buf, size_t len, int flags);
ssize_t inet6_sendto(socket_t *sock, const void *buf, size_t len, int flags,
                     struct sockaddr *dest_addr, int addrlen);
ssize_t inet6_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                       struct sockaddr *src_addr, int *addrlen);
int     inet6_getsockname(socket_t *sock, struct sockaddr *addr, int *addrlen);
int     inet6_getpeername(socket_t *sock, struct sockaddr *addr, int *addrlen);
void    inet6_destroy(socket_t *sock);

/* ── Bridge: called from ipv6.c when a TCP/UDP packet arrives ────────────── */
void tcp6_handle_packet(const uint8_t *payload, uint16_t len,
                        const uint8_t *src6, const uint8_t *dst6);
void udp6_handle_packet(const uint8_t *payload, uint16_t len,
                        const uint8_t *src6, const uint8_t *dst6);

#endif /* AF_INET6_H */
