#include "net/tcp.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "net/ipv4.h"
#include "net/ipv6.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "socket/epoll.h"
#include "fs/vfs.h"

#define FIN     0x01
#define SYN     0x02
#define RST     0x04
#define ACK     0x10
#define PSH     0x08

#define RTO     500
#define RETRIES 5
#define EPHEMERAL_MIN 49152
#define EPHEMERAL_MAX 65535

static struct tcp_tcb tcbs[TCP_MAX_TCBS];
static struct tcp_stats stats;
static spinlock_t lock = SPINLOCK_INIT;
static uint16_t next_ephemeral = EPHEMERAL_MIN;

static uint16_t g16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t g32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           p[3];
}

static void p16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void p32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t csum(uint32_t s, uint32_t d, const uint8_t *p, size_t n)
{
    uint32_t x;

    x = (s >> 16) + (s & 0xffff) +
        (d >> 16) + (d & 0xffff) +
        6 + (uint32_t)n;

    while (n > 1) {
        x += g16(p);
        p += 2;
        n -= 2;
    }

    if (n)
        x += (uint32_t)*p << 8;

    while (x >> 16)
        x = (x & 0xffff) + (x >> 16);

    return (uint16_t)~x;
}

static uint16_t csum6(const uint8_t s[16], const uint8_t d[16],
                      const uint8_t *p, size_t n)
{
    uint32_t x = 6 + (uint32_t)n;
    for (int i = 0; i < 16; i += 2) {
        x += g16(s + i);
        x += g16(d + i);
    }
    size_t left = n;
    while (left > 1) { x += g16(p); p += 2; left -= 2; }
    if (left) x += (uint32_t)*p << 8;
    while (x >> 16) x = (x & 0xffff) + (x >> 16);
    return (uint16_t)~x;
}

static struct tcp_tcb *find(uint32_t ip, uint16_t sp, uint16_t dp)
{
    for (size_t i = 0; i < TCP_MAX_TCBS; i++) {
        if (tcbs[i].used && tcbs[i].address_family != 6 &&
            tcbs[i].remote_ip == ip &&
            tcbs[i].remote_port == sp &&
            tcbs[i].local_port == dp) {
            return &tcbs[i];
        }
    }

    return NULL;
}

static struct tcp_tcb *find6(const uint8_t ip[16], uint16_t sp, uint16_t dp)
{
    for (size_t i = 0; i < TCP_MAX_TCBS; i++)
        if (tcbs[i].used && tcbs[i].address_family == 6 &&
            !memcmp(tcbs[i].remote_ip6, ip, 16) &&
            tcbs[i].remote_port == sp && tcbs[i].local_port == dp)
            return &tcbs[i];
    return NULL;
}

static struct tcp_tcb *alloc_locked(void)
{
    for (size_t i = 0; i < TCP_MAX_TCBS; i++) {
        if (!tcbs[i].used) {
            memset(&tcbs[i], 0, sizeof(tcbs[i]));

            tcbs[i].used = true;
            tcbs[i].mss = 1460;
            tcbs[i].rcv_wnd = TCP_DEFAULT_WINDOW;

            return &tcbs[i];
        }
    }

    return NULL;
}

struct tcp_tcb *tcp_alloc(void)
{
    struct tcp_tcb *t;

    spinlock_acquire(&lock);
    t = alloc_locked();
    spinlock_release(&lock);

    return t;
}

static uint16_t alloc_ephemeral(void)
{
    uint16_t port = 0;

    spinlock_acquire(&lock);

    for (uint32_t n = 0; n <= EPHEMERAL_MAX - EPHEMERAL_MIN; n++) {
        uint16_t candidate = next_ephemeral;
        bool used = false;

        next_ephemeral = candidate == EPHEMERAL_MAX
                             ? EPHEMERAL_MIN
                             : (uint16_t)(candidate + 1);

        for (size_t i = 0; i < TCP_MAX_TCBS; i++) {
            if (tcbs[i].used && tcbs[i].local_port == candidate) {
                used = true;
                break;
            }
        }

        if (!used) {
            port = candidate;
            break;
        }
    }

    spinlock_release(&lock);
    return port;
}

void tcp_free(struct tcp_tcb *t)
{
    if (!t)
        return;

    spinlock_acquire(&lock);
    memset(t, 0, sizeof(*t));
    spinlock_release(&lock);
}

static int emit_at(
    struct tcp_tcb *t,
    uint32_t seq,
    uint8_t flags,
    const void *data,
    size_t len
)
{
    uint8_t segment[1480];

    if (!t || len > 1460)
        return -90;

    memset(segment, 0, 20 + len);

    p16(segment,      t->local_port);
    p16(segment + 2,  t->remote_port);
    p32(segment + 4,  seq);
    p32(segment + 8,  t->rcv_nxt);

    segment[12] = 0x50;
    segment[13] = flags;

    p16(segment + 14,
        t->rcv_wnd ? t->rcv_wnd : TCP_DEFAULT_WINDOW);

    if (len)
        memcpy(segment + 20, data, len);

    if (t->address_family == 6) {
        p16(segment + 16, csum6(t->local_ip6, t->remote_ip6,
                                segment, 20 + len));
        return ipv6_send_raw(t->remote_ip6, 6, segment, 20 + len);
    }
    p16(segment + 16, csum(t->local_ip, t->remote_ip, segment, 20 + len));
    return ipv4_send_raw(t->remote_ip, 6, segment, 20 + len);
}

static int emit(struct tcp_tcb *t, uint8_t flags, const void *data, size_t len)
{
    return emit_at(t, t->snd_nxt, flags, data, len);
}

static int tracked(struct tcp_tcb *t, uint8_t flags, const void *data, size_t len)
{
    t->tx_seq = t->snd_nxt;
    t->tx_flags = flags;
    t->tx_length = len;

    if (len)
        memcpy(t->tx_buffer, data, len);

    return emit_at(t, t->tx_seq, flags, data, len);
}

static void wake(struct tcp_tcb *t)
{
    if (t->wait_queue)
        wait_queue_wake_all((wait_queue_t *)t->wait_queue);

    if (t->vfs_node)
        epoll_notify_event((vfs_node_t *)t->vfs_node, 0x1 | 0x4);
}

int tcp_bind(struct tcp_tcb *t, uint32_t ip, uint16_t port)
{
    if (!t)
        return -22;

    if (!port)
        port = (uint16_t)(49152 + (t - tcbs));

    spinlock_acquire(&lock);

    for (size_t i = 0; i < TCP_MAX_TCBS; i++) {
        if (&tcbs[i] != t &&
            tcbs[i].used &&
            tcbs[i].local_port == port) {
            spinlock_release(&lock);
            return -98;
        }
    }

    t->address_family = 4;
    t->local_ip = ip;
    t->local_port = port;

    spinlock_release(&lock);
    return 0;
}

int tcp_bind6(struct tcp_tcb *t, const uint8_t ip[16], uint16_t port)
{
    if (!t || !ip) return -22;
    if (!port) port = alloc_ephemeral();
    if (!port) return -98;
    spinlock_acquire(&lock);
    for (size_t i = 0; i < TCP_MAX_TCBS; i++)
        if (&tcbs[i] != t && tcbs[i].used && tcbs[i].address_family == 6 &&
            tcbs[i].local_port == port) {
            spinlock_release(&lock); return -98;
        }
    t->address_family = 6; memcpy(t->local_ip6, ip, 16); t->local_port = port;
    spinlock_release(&lock); return 0;
}

int tcp_active_open(struct tcp_tcb *t, uint32_t ip, uint16_t port)
{
    const struct ipv4_config *cfg;
    int r;

    if (!t || !ip || !port)
        return -22;

    cfg = ipv4_get_config();
    if (!cfg || !cfg->address)
        return -101;

    t->address_family = 4;
    t->local_ip = cfg->address;
    t->remote_ip = ip;
    t->remote_port = port;

    if (!t->local_port) {
        t->local_port = alloc_ephemeral();
        if (!t->local_port)
            return -98;
    }

    t->snd_una = 0x90000000u + (uint32_t)(t - tcbs) * 4096u;
    t->snd_nxt = t->snd_una;
    t->state = TCP_SYN_SENT;

    r = tracked(t, SYN, NULL, 0);
    if (r < 0)
        return r;

    t->snd_nxt++;
    t->deadline = lapic_timer_get_ticks() + RTO;

    return 0;
}

int tcp_active_open6(struct tcp_tcb *t, const uint8_t ip[16], uint16_t port)
{
    if (!t || !ip || !port) return -22;
    const struct ipv6_config *cfg = ipv6_get_config();
    bool link = ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80;
    if (!link && !cfg->global_valid) return -101;
    t->address_family = 6;
    memcpy(t->local_ip6, link ? cfg->link_local : cfg->global, 16);
    memcpy(t->remote_ip6, ip, 16); t->remote_port = port;
    if (!t->local_port) { t->local_port = alloc_ephemeral(); if (!t->local_port) return -98; }
    t->snd_una = 0x98000000u + (uint32_t)(t - tcbs) * 4096u;
    t->snd_nxt = t->snd_una; t->state = TCP_SYN_SENT;
    int r = tracked(t, SYN, NULL, 0); if (r < 0) return r;
    t->snd_nxt++; t->deadline = lapic_timer_get_ticks() + RTO; return 0;
}

bool tcp_readable(const struct tcp_tcb *t)
{
    return t && (
        t->rx_head != t->rx_tail ||
        t->peer_closed ||
        t->error
    );
}

bool tcp_writable(const struct tcp_tcb *t)
{
    return t &&
           t->state == TCP_ESTABLISHED &&
           !t->error;
}

int tcp_send(struct tcp_tcb *t, const void *buf, size_t len)
{
    size_t chunk;
    int r;

    if (!t || !buf)
        return -22;

    if (t->state != TCP_ESTABLISHED)
        return -107;

    chunk = len > t->mss ? t->mss : len;

    r = tracked(t, ACK | PSH, buf, chunk);
    if (r < 0)
        return r;

    t->snd_nxt += (uint32_t)chunk;
    t->deadline = lapic_timer_get_ticks() + RTO;

    return (int)chunk;
}

int tcp_recv(struct tcp_tcb *t, void *buf, size_t len, bool nonblock)
{
    for (;;) {
        size_t avail;

        if (!t || !buf)
            return -22;

        spinlock_acquire(&lock);

        avail = t->rx_head - t->rx_tail;

        if (avail) {
            size_t count = avail < len ? avail : len;

            for (size_t i = 0; i < count; i++) {
                ((uint8_t *)buf)[i] =
                    t->rx_buffer[(t->rx_tail + i) % TCP_RX_BUFFER_SIZE];
            }

            t->rx_tail += count;

            spinlock_release(&lock);
            return (int)count;
        }

        if (t->peer_closed) {
            spinlock_release(&lock);
            return 0;
        }

        int err = t->error;

        spinlock_release(&lock);

        if (err)
            return -err;

        if (nonblock)
            return -11;

        struct thread *cur = sched_get_current();

        if (cur && (cur->pending_signals & ~cur->signal_mask))
            return -4;

        sched_yield();
    }
}

int tcp_close(struct tcp_tcb *t)
{
    int r;

    if (!t)
        return -22;

    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
        r = tracked(t, FIN | ACK, NULL, 0);
        if (r < 0)
            return r;

        t->snd_nxt++;

        if (t->state == TCP_CLOSE_WAIT)
            t->state = TCP_LAST_ACK;
        else
            t->state = TCP_FIN_WAIT_1;

        t->deadline = lapic_timer_get_ticks() + RTO;
    }

    return 0;
}

int tcp_listen(struct tcp_tcb *t, int backlog)
{
    int r;

    if (!t)
        return -22;

    if (!t->local_port) {
        r = tcp_bind(t, 0, 0);
        if (r < 0)
            return r;
    }

    if (backlog < 1)
        t->backlog = 1;
    else if (backlog > 8)
        t->backlog = 8;
    else
        t->backlog = backlog;

    t->state = TCP_LISTEN;

    return 0;
}

struct tcp_tcb *tcp_accept(struct tcp_tcb *t, bool nonblock)
{
    if (!t || t->state != TCP_LISTEN)
        return NULL;

    for (;;) {
        spinlock_acquire(&lock);

        if (t->accept_tail != t->accept_head) {
            struct tcp_tcb *child;

            child = t->accept_queue[t->accept_tail % 8];
            t->accept_tail++;

            spinlock_release(&lock);
            return child;
        }

        spinlock_release(&lock);

        if (nonblock)
            return NULL;

        struct thread *cur = sched_get_current();

        if (cur && (cur->pending_signals & ~cur->signal_mask))
            return NULL;

        sched_yield();
    }
}

static void input(
    struct tcp_tcb *t,
    uint32_t seq,
    uint32_t ack,
    uint8_t flags,
    const void *payload,
    size_t len
)
{
    if (flags & RST) {
        t->error = t->state == TCP_SYN_SENT ? 111 : 104;
        t->state = TCP_RESET;
        t->deadline = 0;

        stats.resets++;

        wake(t);
        return;
    }

    if (t->state == TCP_SYN_SENT) {
        if ((flags & (SYN | ACK)) == (SYN | ACK) &&
            ack == t->snd_nxt) {
            t->snd_una = ack;
            t->rcv_nxt = seq + 1;
            t->state = TCP_ESTABLISHED;
            t->retries = 0;
            t->deadline = 0;

            emit(t, ACK, NULL, 0);
            wake(t);
        }

        return;
    }

    if (t->state == TCP_SYN_RECEIVED) {
        if ((flags & ACK) && ack == t->snd_nxt) {
            t->state = TCP_ESTABLISHED;
            t->snd_una = ack;
            t->deadline = 0;

            if (t->listener &&
                t->listener->accept_head - t->listener->accept_tail <
                    (size_t)t->listener->backlog) {
                t->listener->accept_queue[t->listener->accept_head % 8] = t;
                t->listener->accept_head++;

                wake(t->listener);
            }
        }

        return;
    }

    if (seq < t->rcv_nxt) {
        stats.duplicates++;
        return;
    }

    if (seq > t->rcv_nxt) {
        stats.out_of_order++;
        return;
    }

    if ((flags & ACK) &&
        ack > t->snd_una &&
        ack <= t->snd_nxt) {
        t->snd_una = ack;

        if (ack >= t->snd_nxt) {
            t->deadline = 0;
            t->tx_length = 0;
            t->tx_flags = 0;

            wake(t);
        }
    }

    if (len) {
        size_t space = TCP_RX_BUFFER_SIZE - (t->rx_head - t->rx_tail);
        size_t count = len < space ? len : space;

        for (size_t i = 0; i < count; i++) {
            t->rx_buffer[(t->rx_head + i) % TCP_RX_BUFFER_SIZE] =
                ((const uint8_t *)payload)[i];
        }

        t->rx_head += count;
        t->rcv_nxt += (uint32_t)count;

        emit(t, ACK, NULL, 0);
        wake(t);
    }

    if (flags & FIN) {
        t->rcv_nxt++;

        if (t->state == TCP_ESTABLISHED)
            t->state = TCP_CLOSE_WAIT;
        else
            t->state = TCP_TIME_WAIT;

        t->deadline = lapic_timer_get_ticks() + 2 * RTO;

        wake(t);
    } else if (t->state == TCP_FIN_WAIT_1 && ack == t->snd_nxt) {
        t->state = TCP_FIN_WAIT_2;
    }
}

void tcp_input_ipv4(uint32_t s, uint32_t d, const uint8_t *p, size_t len)
{
    size_t header_len;
    struct tcp_tcb *t;

    if (!p || len < 20) {
        stats.malformed++;
        return;
    }

    header_len = (size_t)(p[12] >> 4) * 4;

    if (header_len < 20 || header_len > len) {
        stats.malformed++;
        return;
    }

    if (csum(s, d, p, len)) {
        stats.bad_checksum++;
        return;
    }

    spinlock_acquire(&lock);

    t = find(s, g16(p), g16(p + 2));

    if (t) {
        stats.rx_segments++;

        t->snd_wnd = g16(p + 14);

        input(
            t,
            g32(p + 4),
            g32(p + 8),
            p[13],
            p + header_len,
            len - header_len
        );
    } else if (p[13] & SYN) {
        for (size_t i = 0; i < TCP_MAX_TCBS; i++) {
            if (tcbs[i].used &&
                tcbs[i].state == TCP_LISTEN &&
                tcbs[i].local_port == g16(p + 2)) {
                struct tcp_tcb *child = alloc_locked();

                if (child) {
                    child->listener = &tcbs[i];

                    child->local_ip = d;
                    child->remote_ip = s;

                    child->local_port = g16(p + 2);
                    child->remote_port = g16(p);

                    child->rcv_nxt = g32(p + 4) + 1;

                    child->snd_una =
                        0xa0000000u + (uint32_t)(child - tcbs) * 4096u;
                    child->snd_nxt = child->snd_una;

                    child->state = TCP_SYN_RECEIVED;

                    tracked(child, SYN | ACK, NULL, 0);

                    child->snd_nxt++;
                    child->deadline = lapic_timer_get_ticks() + RTO;
                }

                break;
            }
        }
    }

    spinlock_release(&lock);
}

void tcp_input_ipv6(const uint8_t s[16], const uint8_t d[16],
                    const uint8_t *p, size_t len)
{
    if (!p || len < 20) { stats.malformed++; return; }
    size_t header_len = (size_t)(p[12] >> 4) * 4;
    if (header_len < 20 || header_len > len) { stats.malformed++; return; }
    if (csum6(s, d, p, len)) { stats.bad_checksum++; return; }

    spinlock_acquire(&lock);
    struct tcp_tcb *t = find6(s, g16(p), g16(p + 2));
    if (t) {
        stats.rx_segments++;
        t->snd_wnd = g16(p + 14);
        input(t, g32(p + 4), g32(p + 8), p[13],
              p + header_len, len - header_len);
    } else if (p[13] & SYN) {
        for (size_t i = 0; i < TCP_MAX_TCBS; i++) {
            if (!tcbs[i].used || tcbs[i].address_family != 6 ||
                tcbs[i].state != TCP_LISTEN ||
                tcbs[i].local_port != g16(p + 2))
                continue;
            struct tcp_tcb *child = alloc_locked();
            if (child) {
                child->address_family = 6;
                child->listener = &tcbs[i];
                memcpy(child->local_ip6, d, 16);
                memcpy(child->remote_ip6, s, 16);
                child->local_port = g16(p + 2);
                child->remote_port = g16(p);
                child->rcv_nxt = g32(p + 4) + 1;
                child->snd_una =
                    0xa8000000u + (uint32_t)(child - tcbs) * 4096u;
                child->snd_nxt = child->snd_una;
                child->state = TCP_SYN_RECEIVED;
                tracked(child, SYN | ACK, NULL, 0);
                child->snd_nxt++;
                child->deadline = lapic_timer_get_ticks() + RTO;
            }
            break;
        }
    }
    spinlock_release(&lock);
}


void tcp_timer_tick(uint64_t now)
{
    spinlock_acquire(&lock);

    for (size_t i = 0; i < TCP_MAX_TCBS; i++) {
        struct tcp_tcb *t = &tcbs[i];

        if (!t->used || !t->deadline || now < t->deadline)
            continue;

        if (t->state == TCP_TIME_WAIT) {
            memset(t, 0, sizeof(*t));
            continue;
        }

        if (++t->retries > RETRIES) {
            t->state = TCP_CLOSED;
            t->deadline = 0;
            t->error = 110;

            stats.timeouts++;

            wake(t);
        } else {
            emit_at(
                t,
                t->tx_seq,
                t->tx_flags,
                t->tx_length ? t->tx_buffer : NULL,
                t->tx_length
            );

            t->deadline =
                now + ((uint64_t)RTO << (t->retries > 4 ? 4 : t->retries));

            stats.retransmits++;
        }
    }

    spinlock_release(&lock);
}

const struct tcp_stats *tcp_get_stats(void)
{
    return &stats;
}


void tcp_init(void)
{
    spinlock_init(&lock);

    memset(tcbs, 0, sizeof(tcbs));
    memset(&stats, 0, sizeof(stats));
}

bool net_phase8_init(void)
{
    tcp_init();
    klog_puts("[NET] TCP initialized\n");
    return true;
}

int tcp_get_snapshot(struct tcp_entry_snapshot *out, int max) {
    int n = 0;
    for (int i = 0; i < TCP_MAX_TCBS && n < max; i++) {
        if (!tcbs[i].used || tcbs[i].address_family == 6)
            continue;
        out[n].local_ip    = tcbs[i].local_ip;
        out[n].remote_ip   = tcbs[i].remote_ip;
        out[n].local_port  = tcbs[i].local_port;
        out[n].remote_port = tcbs[i].remote_port;
        out[n].state       = (uint8_t)tcbs[i].state;
        n++;
    }
    return n;
}
