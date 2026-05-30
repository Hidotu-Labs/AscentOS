/*
 * AF_INET6 socket family — IPv6 TCP and UDP support.
 * Mirrors af_inet.c but uses 128-bit addresses and ipv6_send_packet().
 */
#include "af_inet6.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../net/byteorder.h"
#include "../net/ipv6.h"
#include "../net/net.h"
#include "../net/tcp.h"
#include "../net/udp.h"
#include "../sched/sched.h"
#include "socket_internal.h"

/* ── TCP/UDP over IPv6: separate socket tables ──────────────────────────── */
/* We reuse the existing tcp_socket_t / udp_socket_t infrastructure but
 * store the 128-bit addresses separately in inet6_sock_t.               */

/* Map tcp_sock_id → inet6_sock_t* (parallel to af_inet's tcp_inet_map) */
static inet6_sock_t *tcp6_map[MAX_TCP_SOCKETS];
static sk_buff_t    *early6_data[MAX_TCP_SOCKETS];

/* UDP: map udp slot index → inet6_sock_t* */
#define MAX_UDP6_SOCKETS 16
static inet6_sock_t *udp6_map[MAX_UDP6_SOCKETS];
/* udp6 port → inet6_sock_t* lookup by port */

/* ── Family registration ─────────────────────────────────────────────────── */
static net_family_t inet6_family_ops;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static inline void addr6_copy(uint8_t *dst, const uint8_t *src) {
    memcpy(dst, src, 16);
}
static inline int addr6_eq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 16) == 0;
}

/* ── TCP bridge callbacks ─────────────────────────────────────────────────── */
static void tcp6_data_cb(int sock_id, const uint8_t *data, uint16_t len) {
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS) return;
    inet6_sock_t *inet = tcp6_map[sock_id];
    if (!inet) {
        if (!data || len == 0) return;
        sk_buff_t *skb = alloc_skb(len);
        if (!skb) return;
        memcpy(skb->data, data, len);
        skb->len = len; skb->next = NULL;
        if (!early6_data[sock_id]) {
            early6_data[sock_id] = skb;
        } else {
            sk_buff_t *t = early6_data[sock_id];
            while (t->next) t = t->next;
            t->next = skb;
        }
        return;
    }
    if (data && len > 0) {
        sk_buff_t *skb = alloc_skb(len);
        if (skb) {
            memcpy(skb->data, data, len);
            skb->len = len;
            skb_queue_tail(&inet->receive_queue, skb);
        }
    } else if (len == 0) {
        if (inet->parent) inet->parent->state = SS_UNCONNECTED;
    }
    if (inet->parent) socket_wake(inet->parent);
}

static void tcp6_event_cb(int sock_id) {
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS) return;
    inet6_sock_t *inet = tcp6_map[sock_id];
    if (inet && inet->parent) socket_wake(inet->parent);
}

/* ── UDP bridge callback ─────────────────────────────────────────────────── */
static void udp6_data_cb(uint16_t local_port, const uint8_t *data,
                         uint16_t len, uint32_t src_ip, uint16_t src_port) {
    /* src_ip is unused for IPv6 — we stored src in skb via udp6_handle_packet */
    (void)src_ip;
    inet6_sock_t *inet = NULL;
    for (int i = 0; i < MAX_UDP6_SOCKETS; i++) {
        if (udp6_map[i] && udp6_map[i]->udp_port == local_port) {
            inet = udp6_map[i]; break;
        }
    }
    if (!inet) return;
    sk_buff_t *skb = alloc_skb(len);
    if (skb) {
        memcpy(skb->data, data, len);
        skb->len = len;
        skb->src_port = src_port;
        /* src IPv6 address stored separately — not in skb for now */
        skb_queue_tail(&inet->receive_queue, skb);
    }
    if (inet->parent) socket_wake(inet->parent);
}

/* ── Public bridge: called from ipv6.c ──────────────────────────────────── */
void tcp6_handle_packet(const uint8_t *payload, uint16_t len,
                        const uint8_t *src6, const uint8_t *dst6) {
    /* tcp_handle_packet uses 32-bit IPs for socket matching.
     * For IPv6 we need to match by port only (src6 stored in inet6_sock).
     * We pass 0 for IPs and rely on port matching + state machine.       */
    (void)dst6;
    /* Pack last 4 bytes of src6 as a pseudo src_ip for port matching */
    uint32_t pseudo_src = ((uint32_t)src6[12] << 24) | ((uint32_t)src6[13] << 16)
                        | ((uint32_t)src6[14] << 8)  |  (uint32_t)src6[15];
    tcp_handle_packet(payload, len, pseudo_src, 0);
}

void udp6_handle_packet(const uint8_t *payload, uint16_t len,
                        const uint8_t *src6, const uint8_t *dst6) {
    (void)dst6;
    if (len < 8) return;
    uint16_t src_port = (uint16_t)((payload[0] << 8) | payload[1]);
    uint16_t dst_port = (uint16_t)((payload[2] << 8) | payload[3]);
    uint16_t udp_len  = (uint16_t)((payload[4] << 8) | payload[5]);
    if (len < udp_len) return;
    const uint8_t *data = payload + 8;
    uint16_t data_len = udp_len - 8;
    /* Find a registered udp6 socket */
    for (int i = 0; i < MAX_UDP6_SOCKETS; i++) {
        if (udp6_map[i] && udp6_map[i]->udp_port == dst_port) {
            inet6_sock_t *inet = udp6_map[i];
            sk_buff_t *skb = alloc_skb(data_len);
            if (skb) {
                memcpy(skb->data, data, data_len);
                skb->len = data_len;
                skb->src_port = src_port;
                /* Store src IPv6 in src_ip as best-effort (last 4 bytes) */
                skb->src_ip = ((uint32_t)src6[12] << 24) | ((uint32_t)src6[13] << 16)
                            | ((uint32_t)src6[14] << 8)  |  (uint32_t)src6[15];
                skb_queue_tail(&inet->receive_queue, skb);
            }
            if (inet->parent) socket_wake(inet->parent);
            return;
        }
    }
    /* Also try the kernel UDP table (for DNS etc.) */
    udp_handle_packet(payload, len, 0, 0);
}

/* ── sock_ops_t forward declarations ─────────────────────────────────────── */
static sock_ops_t inet6_stream_ops;
static sock_ops_t inet6_dgram_ops;

/* ── inet6_create ────────────────────────────────────────────────────────── */
int inet6_create(socket_t *sock, int protocol) {
    (void)protocol;
    inet6_sock_t *inet = kmalloc(sizeof(inet6_sock_t));
    if (!inet) return -12;
    memset(inet, 0, sizeof(inet6_sock_t));
    inet->parent = sock;
    inet->tcp_sock_id = -1;
    spinlock_init(&inet->receive_queue.lock);
    sock->sk = inet;
    if (sock->type == SOCK_STREAM)
        sock->ops = &inet6_stream_ops;
    else if (sock->type == SOCK_DGRAM)
        sock->ops = &inet6_dgram_ops;
    else { kfree(inet); return -93; }
    return 0;
}

/* ── inet6_bind ──────────────────────────────────────────────────────────── */
int inet6_bind(socket_t *sock, struct sockaddr *addr, int addrlen) {
    if (addrlen < (int)sizeof(struct sockaddr_in6)) return -22;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    memcpy(&inet->local_addr, sin6, sizeof(struct sockaddr_in6));

    if (sock->type == SOCK_DGRAM) {
        uint16_t port = ntohs(sin6->sin6_port);
        int bp = udp_bind(port, udp6_data_cb);
        if (bp < 0) return -98;
        inet->udp_port = (uint16_t)bp;
        inet->local_addr.sin6_port = htons(inet->udp_port);
        for (int i = 0; i < MAX_UDP6_SOCKETS; i++) {
            if (!udp6_map[i]) { udp6_map[i] = inet; break; }
        }
    }
    return 0;
}

/* ── inet6_connect ───────────────────────────────────────────────────────── */
int inet6_connect(socket_t *sock, struct sockaddr *addr, int addrlen) {
    if (addrlen < (int)sizeof(struct sockaddr_in6)) return -22;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    memcpy(&inet->remote_addr, sin6, sizeof(struct sockaddr_in6));

    uint16_t port = ntohs(sin6->sin6_port);
    const uint8_t *a = sin6->sin6_addr.s6_addr;

    if (sock->type == SOCK_DGRAM) {
        if (inet->udp_port == 0) {
            int bp = udp_bind(0, udp6_data_cb);
            if (bp < 0) return -98;
            inet->udp_port = (uint16_t)bp;
            inet->local_addr.sin6_port = htons(inet->udp_port);
            for (int i = 0; i < MAX_UDP6_SOCKETS; i++) {
                if (!udp6_map[i]) { udp6_map[i] = inet; break; }
            }
        }
        sock->state = SS_CONNECTED;
        return 0;
    }

    /* Check for IPv4-mapped IPv6 address: ::ffff:x.x.x.x
     * Bytes 0-9 = 0x00, bytes 10-11 = 0xFF, bytes 12-15 = IPv4 addr */
    static const uint8_t v4mapped_pfx[12] = {
        0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF
    };
    uint32_t ipv4_addr = 0;
    bool is_v4mapped = (memcmp(a, v4mapped_pfx, 12) == 0);
    if (is_v4mapped) {
        ipv4_addr = ((uint32_t)a[12] << 24) | ((uint32_t)a[13] << 16)
                  | ((uint32_t)a[14] << 8)  |  (uint32_t)a[15];
    }

    /* Reject unroutable addresses: all-zeros, loopback (::1), or
     * pure IPv6 global addresses we can't route via our IPv4-only stack. */
    bool all_zero = true;
    for (int i = 0; i < 16; i++) { if (a[i]) { all_zero = false; break; } }
    if (all_zero) return -101; /* ENETUNREACH */

    /* ::1 loopback — map to 127.0.0.1 */
    static const uint8_t loopback6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (memcmp(a, loopback6, 16) == 0) {
        ipv4_addr = 0x7F000001;
        is_v4mapped = true;
    }

    /* For pure IPv6 global addresses (not mapped), we don't have IPv6
     * routing — return ENETUNREACH so the caller falls back to IPv4. */
    if (!is_v4mapped) {
        return -101; /* ENETUNREACH */
    }

    sock->state = SS_CONNECTING;
    int sock_id = tcp_connect(ipv4_addr, port, tcp6_data_cb);
    if (sock_id < 0) { sock->state = SS_UNCONNECTED; return sock_id; }

    inet->tcp_sock_id = sock_id;
    tcp6_map[sock_id] = inet;
    tcp_set_callbacks(sock_id, tcp6_data_cb, tcp6_event_cb);
    net_poll();
    sock->state = SS_CONNECTED;
    return 0;
}

/* ── inet6_listen / accept ───────────────────────────────────────────────── */
int inet6_listen(socket_t *sock, int backlog) {
    (void)backlog;
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    uint16_t port = ntohs(inet->local_addr.sin6_port);
    if (!port) return -22;
    int sid = tcp_listen(port, tcp6_data_cb);
    if (sid < 0) return -98;
    inet->tcp_sock_id = sid;
    tcp6_map[sid] = inet;
    tcp_set_callbacks(sid, tcp6_data_cb, tcp6_event_cb);
    sock->state = SS_LISTENING;
    return 0;
}

int inet6_accept(socket_t *sock, socket_t **newsock) {
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    if (inet->tcp_sock_id < 0) return -22;
    int new_id = -1;
    while ((new_id = tcp_accept(inet->tcp_sock_id)) < 0) {
        if (sock->flags & SOCK_NONBLOCK) return -11;
        net_poll(); sched_yield();
    }
    socket_t *ns = socket_create(AF_INET6, SOCK_STREAM, 0);
    if (!ns) return -12;
    inet6_sock_t *ni = (inet6_sock_t *)ns->sk;
    ni->tcp_sock_id = new_id;
    tcp6_map[new_id] = ni;
    tcp_set_callbacks(new_id, tcp6_data_cb, tcp6_event_cb);
    while (early6_data[new_id]) {
        sk_buff_t *skb = early6_data[new_id];
        early6_data[new_id] = skb->next; skb->next = NULL;
        skb_queue_tail(&ni->receive_queue, skb);
    }
    ns->state = SS_CONNECTED;
    *newsock = ns;
    return 0;
}

/* ── poll ────────────────────────────────────────────────────────────────── */
int inet6_poll(socket_t *sock, int events) {
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    int rev = 0;
    if ((events & POLLIN) && !skb_queue_empty(&inet->receive_queue))
        rev |= POLLIN;
    if (events & POLLOUT) {
        if (sock->type == SOCK_DGRAM || sock->state == SS_CONNECTED)
            rev |= POLLOUT;
    }
    return rev;
}

/* ── send / recv ─────────────────────────────────────────────────────────── */
ssize_t inet6_send(socket_t *sock, const void *buf, size_t len, int flags) {
    (void)flags;
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    if (inet->tcp_sock_id < 0) return -107;
    size_t sent = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (sent < len) {
        uint16_t chunk = (len - sent) > 1400 ? 1400 : (uint16_t)(len - sent);
        int r = tcp_send(inet->tcp_sock_id, p + sent, chunk);
        if (r < 0) return sent > 0 ? (ssize_t)sent : -104;
        sent += r;
    }
    return (ssize_t)sent;
}

ssize_t inet6_recv(socket_t *sock, void *buf, size_t len, int flags) {
    return inet6_recvfrom(sock, buf, len, flags, NULL, NULL);
}

ssize_t inet6_sendto(socket_t *sock, const void *buf, size_t len, int flags,
                     struct sockaddr *dest_addr, int addrlen) {
    (void)flags;
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    if (sock->type == SOCK_STREAM) return inet6_send(sock, buf, len, flags);
    if (!dest_addr || addrlen < (int)sizeof(struct sockaddr_in6)) return -22;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)dest_addr;
    uint16_t dst_port = ntohs(sin6->sin6_port);

    if (inet->udp_port == 0) {
        int bp = udp_bind(0, udp6_data_cb);
        if (bp < 0) return -105;
        inet->udp_port = (uint16_t)bp;
        for (int i = 0; i < MAX_UDP6_SOCKETS; i++) {
            if (!udp6_map[i]) { udp6_map[i] = inet; break; }
        }
    }

    /* Build UDP packet and send via IPv6 */
    uint16_t udp_len = 8 + (uint16_t)len;
    uint8_t pkt[udp_len];
    pkt[0] = inet->udp_port >> 8; pkt[1] = inet->udp_port & 0xFF;
    pkt[2] = dst_port >> 8;       pkt[3] = dst_port & 0xFF;
    pkt[4] = udp_len >> 8;        pkt[5] = udp_len & 0xFF;
    pkt[6] = 0; pkt[7] = 0; /* checksum — optional for IPv6 UDP (RFC 6935) */
    memcpy(pkt + 8, buf, len);

    /* Compute checksum */
    const uint8_t *src6 = ipv6_get_global() ? ipv6_get_global() : ipv6_get_linklocal();
    uint16_t csum = ipv6_checksum(src6, sin6->sin6_addr.s6_addr,
                                  IPV6_PROTO_UDP, udp_len, pkt);
    pkt[6] = csum >> 8; pkt[7] = csum & 0xFF;

    int r = ipv6_send_packet(sin6->sin6_addr.s6_addr, IPV6_PROTO_UDP, pkt, udp_len);
    return r < 0 ? r : (ssize_t)len;
}

ssize_t inet6_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                       struct sockaddr *src_addr, int *addrlen) {
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    bool nonblock = (sock->flags & SOCK_NONBLOCK) || (flags & MSG_DONTWAIT);

    while (skb_queue_empty(&inet->receive_queue)) {
        if (sock->type == SOCK_STREAM && sock->state != SS_CONNECTED
            && sock->state != SS_CONNECTING) return 0;
        if (nonblock) return -11;
        net_poll(); sched_yield();
    }

    sk_buff_t *skb = skb_dequeue(&inet->receive_queue);
    if (!skb) return 0;

    size_t copy = len < skb->len ? len : skb->len;
    memcpy(buf, skb->data, copy);

    if (src_addr && addrlen && *addrlen >= (int)sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)src_addr;
        memset(s6, 0, sizeof(*s6));
        s6->sin6_family = AF_INET6;
        s6->sin6_port   = htons(skb->src_port);
        /* Best-effort: fill last 4 bytes from src_ip */
        s6->sin6_addr.s6_addr[15] = skb->src_ip & 0xFF;
        s6->sin6_addr.s6_addr[14] = (skb->src_ip >> 8) & 0xFF;
        s6->sin6_addr.s6_addr[13] = (skb->src_ip >> 16) & 0xFF;
        s6->sin6_addr.s6_addr[12] = (skb->src_ip >> 24) & 0xFF;
        *addrlen = sizeof(struct sockaddr_in6);
    }

    if (copy < skb->len && sock->type == SOCK_STREAM) {
        size_t rem = skb->len - copy;
        sk_buff_t *rest = alloc_skb(rem);
        if (rest) {
            memcpy(rest->data, skb->data + copy, rem);
            rest->len = rem;
            skb_queue_tail(&inet->receive_queue, rest);
        }
    }
    free_skb(skb);
    return (ssize_t)copy;
}

/* ── getsockname / getpeername ───────────────────────────────────────────── */
int inet6_getsockname(socket_t *sock, struct sockaddr *addr, int *addrlen) {
    if (!addr || !addrlen) return -22;
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    int copy = *addrlen < (int)sizeof(struct sockaddr_in6)
             ? *addrlen : (int)sizeof(struct sockaddr_in6);
    struct sockaddr_in6 out;
    memcpy(&out, &inet->local_addr, sizeof(out));
    if (sock->type == SOCK_DGRAM)
        out.sin6_port = htons(inet->udp_port);
    /* Fill address from our link-local if not set */
    if (ipv6_addr_is_zero(out.sin6_addr.s6_addr)) {
        const uint8_t *ll = ipv6_get_linklocal();
        if (ll) memcpy(out.sin6_addr.s6_addr, ll, 16);
    }
    memcpy(addr, &out, copy);
    *addrlen = sizeof(struct sockaddr_in6);
    return 0;
}

int inet6_getpeername(socket_t *sock, struct sockaddr *addr, int *addrlen) {
    if (!addr || !addrlen) return -22;
    if (sock->state != SS_CONNECTED) return -107;
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    int copy = *addrlen < (int)sizeof(struct sockaddr_in6)
             ? *addrlen : (int)sizeof(struct sockaddr_in6);
    memcpy(addr, &inet->remote_addr, copy);
    *addrlen = sizeof(struct sockaddr_in6);
    return 0;
}

/* ── destroy ─────────────────────────────────────────────────────────────── */
void inet6_destroy(socket_t *sock) {
    inet6_sock_t *inet = (inet6_sock_t *)sock->sk;
    if (!inet) return;

    if (inet->tcp_sock_id >= 0 && inet->tcp_sock_id < MAX_TCP_SOCKETS) {
        while (early6_data[inet->tcp_sock_id]) {
            sk_buff_t *skb = early6_data[inet->tcp_sock_id];
            early6_data[inet->tcp_sock_id] = skb->next;
            free_skb(skb);
        }
        tcp6_map[inet->tcp_sock_id] = NULL;
        tcp_close(inet->tcp_sock_id);
        inet->tcp_sock_id = -1;
    }
    if (inet->udp_port > 0) {
        for (int i = 0; i < MAX_UDP6_SOCKETS; i++) {
            if (udp6_map[i] == inet) { udp6_map[i] = NULL; break; }
        }
        udp_unbind(inet->udp_port);
        inet->udp_port = 0;
    }
    sk_buff_t *skb;
    while ((skb = skb_dequeue(&inet->receive_queue)) != NULL) free_skb(skb);
    kfree(inet);
    sock->sk = NULL;
}

/* ── ops tables ──────────────────────────────────────────────────────────── */
static sock_ops_t inet6_stream_ops = {
    .bind        = inet6_bind,
    .connect     = inet6_connect,
    .send        = inet6_send,
    .recv        = inet6_recv,
    .listen      = inet6_listen,
    .accept      = inet6_accept,
    .sendto      = inet6_sendto,
    .recvfrom    = inet6_recvfrom,
    .poll        = inet6_poll,
    .getsockname = inet6_getsockname,
    .getpeername = inet6_getpeername,
    .destroy     = inet6_destroy,
};

static sock_ops_t inet6_dgram_ops = {
    .bind        = inet6_bind,
    .connect     = inet6_connect,
    .send        = inet6_send,
    .recv        = inet6_recv,
    .sendto      = inet6_sendto,
    .recvfrom    = inet6_recvfrom,
    .poll        = inet6_poll,
    .getsockname = inet6_getsockname,
    .getpeername = inet6_getpeername,
    .destroy     = inet6_destroy,
};

/* ── init ────────────────────────────────────────────────────────────────── */
void af_inet6_init(void) {
    memset(tcp6_map,    0, sizeof(tcp6_map));
    memset(early6_data, 0, sizeof(early6_data));
    memset(udp6_map,    0, sizeof(udp6_map));

    inet6_family_ops.family = AF_INET6;
    inet6_family_ops.create = inet6_create;
    inet6_family_ops.next   = NULL;

    sock_register_family(&inet6_family_ops);
    klog_puts("[OK] AF_INET6 protocol family registered\n");
}
