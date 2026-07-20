#include "net/udp.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/heap.h"
#include "net/core.h"
#include "net/ipv4.h"
#include "sched/sched.h"
#include "sched/wait.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static struct udp_socket sockets[UDP_MAX_SOCKETS];
static spinlock_t        table_lock = SPINLOCK_INIT;
static uint16_t          next_ephemeral = UDP_PORT_EPHEMERAL_MIN;

static inline uint16_t be16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint32_t be32(uint32_t v) {
    return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8)  | ((v & 0xff000000u) >> 24);
}
static inline void put32_be(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8);  p[3]=(uint8_t)v;
}
static inline void put16_be(uint8_t *p, uint16_t v) {
    p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v;
}

static uint16_t inet_cksum(const void *d, size_t n) __attribute__((unused));
static uint16_t inet_cksum(const void *d, size_t n) {
    const uint8_t *b = (const uint8_t *)d;
    uint32_t sum = 0;
    while (n > 1) { sum += ((uint32_t)b[0]<<8)|b[1]; b+=2; n-=2; }
    if (n) sum += (uint32_t)b[0]<<8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t udp_cksum(uint32_t src_ip, uint32_t dst_ip,
                           const void *udp_seg, uint16_t udp_len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xffff;
    sum += src_ip & 0xffff;
    sum += (dst_ip >> 16) & 0xffff;
    sum += dst_ip & 0xffff;
    sum += 17u;
    sum += (uint32_t)udp_len;
    const uint8_t *b = (const uint8_t *)udp_seg;
    uint16_t rem = udp_len;
    while (rem > 1) { sum += ((uint32_t)b[0]<<8)|b[1]; b+=2; rem-=2; }
    if (rem) sum += (uint32_t)b[0]<<8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static bool port_in_use(uint16_t port) {
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].bound &&
            sockets[i].local_port == port)
            return true;
    }
    return false;
}

static uint16_t alloc_ephemeral(void) {
    for (uint16_t i = 0; i <= (UDP_PORT_EPHEMERAL_MAX - UDP_PORT_EPHEMERAL_MIN); i++) {
        uint16_t p = (uint16_t)(((next_ephemeral - UDP_PORT_EPHEMERAL_MIN + i) %
                      (UDP_PORT_EPHEMERAL_MAX - UDP_PORT_EPHEMERAL_MIN + 1)) +
                      UDP_PORT_EPHEMERAL_MIN);
        if (!port_in_use(p)) {
            next_ephemeral = (uint16_t)(p == UDP_PORT_EPHEMERAL_MAX
                             ? UDP_PORT_EPHEMERAL_MIN : p + 1);
            return p;
        }
    }
    return 0;
}

void udp_init(void) {
    spinlock_init(&table_lock);
    memset(sockets, 0, sizeof(sockets));
}

struct udp_socket *udp_socket_alloc(void) {
    spinlock_acquire(&table_lock);
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].used = true;
            spinlock_release(&table_lock);
            return &sockets[i];
        }
    }
    spinlock_release(&table_lock);
    return NULL;
}

void udp_socket_free(struct udp_socket *s) {
    if (!s) return;
    spinlock_acquire(&table_lock);
    memset(s, 0, sizeof(*s));
    spinlock_release(&table_lock);
}

int udp_bind(struct udp_socket *s, uint32_t ip, uint16_t port) {
    if (!s) return -22;
    spinlock_acquire(&table_lock);
    if (port != 0 && port_in_use(port)) {
        spinlock_release(&table_lock);
        return -98;
    }
    if (port == 0) {
        port = alloc_ephemeral();
        if (!port) {
            spinlock_release(&table_lock);
            return -98;
        }
    }
    s->local_ip   = ip;
    s->local_port = port;
    s->bound      = true;
    spinlock_release(&table_lock);
    return 0;
}

int udp_connect(struct udp_socket *s, uint32_t ip, uint16_t port) {
    if (!s) return -22;
    if (!s->bound) {
        int r = udp_bind(s, 0, 0);
        if (r < 0) return r;
    }
    s->remote_ip   = ip;
    s->remote_port = port;
    s->connected   = true;
    return 0;
}

#define UDP_HDR_LEN 8

ssize_t udp_sendto(struct udp_socket *s, const void *buf, size_t len,
                   uint32_t dst_ip, uint16_t dst_port) {
    if (!s || !buf) return -22;
    if (len > UDP_PAYLOAD_MAX) return -90;

    if (!s->bound) {
        int r = udp_bind(s, 0, 0);
        if (r < 0) return r;
    }

    const struct ipv4_config *cfg = ipv4_get_config();
    if (!cfg || !cfg->address) return -101;

    uint16_t udp_len = (uint16_t)(UDP_HDR_LEN + len);
    uint8_t *seg = kmalloc(udp_len);
    if (!seg) return -12;
    put16_be(seg + 0, s->local_port);
    put16_be(seg + 2, dst_port);
    put16_be(seg + 4, udp_len);
    seg[6] = 0; seg[7] = 0;
    memcpy(seg + UDP_HDR_LEN, buf, len);

    uint16_t csum = udp_cksum(cfg->address, dst_ip, seg, udp_len);
    put16_be(seg + 6, csum);

    int r = ipv4_send_raw(dst_ip, 17, seg, udp_len);
    kfree(seg);
    return r < 0 ? (ssize_t)r : (ssize_t)len;
}

ssize_t udp_recvfrom(struct udp_socket *s, void *buf, size_t len,
                     uint32_t *src_ip, uint16_t *src_port,
                     bool nonblocking, int timeout_ms) {
    if (!s || !buf) return -22;

    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = lapic_timer_get_ticks() + (uint32_t)timeout_ms;

    for (;;) {
        spinlock_acquire(&table_lock);
        if (s->q_head != s->q_tail) {
            struct udp_rxbuf *rb = &s->queue[s->q_tail % UDP_RX_QUEUE_DEPTH];
            size_t copy = rb->length < len ? rb->length : len;
            memcpy(buf, rb->data, copy);
            if (src_ip)   *src_ip   = rb->src_ip;
            if (src_port) *src_port = rb->src_port;
            s->q_tail++;
            spinlock_release(&table_lock);
            return (ssize_t)(rb->length > len ? len : rb->length);
        }
        spinlock_release(&table_lock);

        if (nonblocking) return -11;
        if (timeout_ms > 0 && lapic_timer_get_ticks() >= deadline) return -11;

        if (s->wait_queue) {
            struct thread *self = sched_get_current();
            wait_queue_t *wq = (wait_queue_t *)s->wait_queue;
            wait_queue_entry_t entry = { .thread = self, .next = NULL };
            wait_queue_add(wq, &entry);
            spinlock_acquire(&table_lock);
            bool empty = s->q_head == s->q_tail;
            if (empty) self->state = THREAD_BLOCKED;
            spinlock_release(&table_lock);
            if (!empty) wait_queue_wake_one(wq);
            sched_yield();
            wait_queue_remove(wq, &entry);
        } else {
            sched_yield();
        }
    }
}

void udp_deliver(uint32_t src_ip, uint16_t src_port,
                 uint16_t dst_port, const uint8_t *payload, uint16_t length) {
    spinlock_acquire(&table_lock);
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        struct udp_socket *s = &sockets[i];
        if (!s->used || !s->bound) continue;
        if (s->local_port != dst_port) continue;
        if (s->local_ip != 0 && s->local_ip != ipv4_get_config()->address) continue;
        if (s->connected && s->remote_ip != src_ip) continue;
        if (s->connected && s->remote_port != src_port) continue;

        uint32_t next_head = (s->q_head + 1) % (UDP_RX_QUEUE_DEPTH * 2);
        if (next_head == s->q_tail % (UDP_RX_QUEUE_DEPTH * 2)) {
            spinlock_release(&table_lock);
            return;
        }

        uint16_t copy = length > UDP_PAYLOAD_MAX ? UDP_PAYLOAD_MAX : length;
        struct udp_rxbuf *rb = &s->queue[s->q_head % UDP_RX_QUEUE_DEPTH];
        memcpy(rb->data, payload, copy);
        rb->length   = copy;
        rb->src_ip   = src_ip;
        rb->src_port = src_port;
        s->q_head++;

        if (s->wait_queue)
            wait_queue_wake_one((wait_queue_t *)s->wait_queue);
        if (s->vfs_node) {
            extern void epoll_notify_event(struct vfs_node *, uint32_t);
            epoll_notify_event((struct vfs_node *)s->vfs_node, 0x00000001u);
        }
        spinlock_release(&table_lock);
        return;
    }
    spinlock_release(&table_lock);
}

bool net_phase6_init(void) {
    udp_init();
    klog_puts("[NET] UDP initialized\n");
    return true;
}

int udp_get_snapshot(struct udp_entry_snapshot *out, int max) {
    int n = 0;
    spinlock_acquire(&table_lock);
    for (int i = 0; i < UDP_MAX_SOCKETS && n < max; i++) {
        if (!sockets[i].used || !sockets[i].bound)
            continue;
        out[n].local_ip    = sockets[i].local_ip;
        out[n].local_port  = sockets[i].local_port;
        out[n].remote_ip   = sockets[i].remote_ip;
        out[n].remote_port = sockets[i].remote_port;
        out[n].connected   = sockets[i].connected;
        n++;
    }
    spinlock_release(&table_lock);
    return n;
}
