#include "af_netlink.h"
#include "socket_internal.h"
#include "epoll.h"
#include "socket.h"
#include "../console/klog.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../sched/sched.h"

static void netlink_push_fake_uevent(socket_t *sock) {
    klog_puts("[NETLINK] push_fake_uevent start\n");
    if (!sock || !sock->sk) return;
    netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
    const char *uevent = "add@/devices/pci0000:00/0000:00:08.1/0000:0e:00.0/drm/card0\0"
                         "ACTION=add\0"
                         "DEVPATH=/devices/pci0000:00/0000:00:08.1/0000:0e:00.0/drm/card0\0"
                         "SUBSYSTEM=drm\0"
                         "DEVNAME=/dev/dri/card0\0"
                         "MAJOR=226\0"
                         "MINOR=0\0"
                         "SEQNUM=100\0";
    size_t uevent_len = 256;
    
    klog_puts("[NETLINK] allocating skb\n");
    sk_buff_t *skb = alloc_skb(uevent_len);
    if (skb) {
        memset(skb->data, 0, uevent_len);
        memcpy(skb->data, uevent, 160);
        skb->len = uevent_len;
        
        // skb_queue_tail handles its own locking, manual locking here causes DEADLOCK
        skb_queue_tail(&nsk->recv_queue, skb);
        
        socket_wake(sock);
        if (sock->node) epoll_notify_event(sock->node, EPOLLIN);
        klog_puts("[NETLINK] Pushed fake uevent for card0\n");
    } else {
        klog_puts("[NETLINK] skb allocation failed\n");
    }
}

// ── AF_NETLINK Operations ───────────────────────────────────────────────────

static int netlink_bind(socket_t *sock, struct sockaddr *addr, int addrlen) {
    if (addrlen < (int)sizeof(struct sockaddr_nl)) return -22; // EINVAL
    struct sockaddr_nl *nl = (struct sockaddr_nl *)addr;
    if (nl->nl_family != AF_NETLINK) return -97; // EAFNOSUPPORT

    netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
    nsk->groups = nl->nl_groups;
    nsk->portid = nl->nl_pid;

    klog_puts("[NETLINK] bind called: port=");
    klog_uint64(nsk->portid);
    klog_puts(" groups=");
    klog_uint64(nsk->groups);
    klog_puts("\n");

    // If udev is binding (portid != 0 or just protocol is uevent), push a fake uevent
    // to notify it about the DRM card and input devices.
    if (nsk->protocol == 15) { // NETLINK_KOBJECT_UEVENT
        netlink_push_fake_uevent(sock);
    }

    return 0;
}

static int netlink_getsockname(socket_t *sock, struct sockaddr *addr, int *addrlen) {
    if (*addrlen < (int)sizeof(struct sockaddr_nl)) return -22;
    netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
    
    struct sockaddr_nl nl;
    memset(&nl, 0, sizeof(nl));
    nl.nl_family = AF_NETLINK;
    nl.nl_pid = (uint32_t)nsk->portid;
    nl.nl_groups = nsk->groups;

    memcpy(addr, &nl, sizeof(nl));
    *addrlen = sizeof(nl);
    return 0;
}


static int netlink_setsockopt(socket_t *sock, int level, int optname, const void *optval, int optlen) {
    if (!sock || !sock->sk) return -22;
    netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;

    klog_puts("[NETLINK] setsockopt: level=");
    klog_uint64((uint64_t)level);
    klog_puts(" optname=");
    klog_uint64((uint64_t)optname);
    klog_puts("\n");

    if (level == SOL_NETLINK) {
        switch (optname) {
            case NETLINK_ADD_MEMBERSHIP: {
                if (optlen < (int)sizeof(int)) return -22;
                int group = *(const int *)optval;
                klog_puts("[NETLINK] setsockopt: ADD_MEMBERSHIP group=");
                klog_uint64((uint64_t)group);
                klog_puts("\n");
                
                uint32_t group_mask = (1 << (group - 1));
                nsk->groups |= group_mask;
                
                if (group == 1) { // 1 is often the uevent group
                    netlink_push_fake_uevent(sock);
                }
                return 0;
            }
            case NETLINK_DROP_MEMBERSHIP: {
                if (optlen < (int)sizeof(int)) return -22;
                int group = *(const int *)optval;
                uint32_t group_mask = (1 << (group - 1));
                nsk->groups &= ~group_mask;
                return 0;
            }
        }
    } else if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_PASSCRED:
                klog_puts("[NETLINK] setsockopt: SO_PASSCRED\n");
                return 0; // Stub success
            case 26: // SO_ATTACH_FILTER
                klog_puts("[NETLINK] setsockopt: SO_ATTACH_FILTER\n");
                return 0; // Stub success
            case 2: // SO_REUSEADDR
                return 0; 
        }
    }

    return -92; // ENOPROTOOPT
}

static int netlink_getsockopt(socket_t *sock, int level, int optname, void *optval, int *optlen) {
    (void)sock;
    (void)optval;
    (void)optlen;
    (void)level;
    (void)optname;
    return -92; // ENOPROTOOPT
}

static ssize_t netlink_recv(socket_t *sock, void *buf, size_t len, int flags) {
    if (!sock || !sock->sk) return -22;
    netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
    klog_puts("[NETLINK] recv tid=");
    struct thread *_curr = sched_get_current();
    if (_curr) klog_uint64(_curr->tid);
    klog_puts("\n");

    spinlock_acquire(&nsk->recv_queue.lock);
    while (skb_queue_empty(&nsk->recv_queue)) {
        if (sock->flags & SOCK_NONBLOCK) {
            spinlock_release(&nsk->recv_queue.lock);
            return -11; // EAGAIN
        }
        spinlock_release(&nsk->recv_queue.lock);
        socket_wait(sock);
        spinlock_acquire(&nsk->recv_queue.lock);
    }

    sk_buff_t *skb = skb_dequeue(&nsk->recv_queue);
    spinlock_release(&nsk->recv_queue.lock);

    if (!skb) return 0;

    size_t to_copy = (len < skb->len) ? len : skb->len;
    memcpy(buf, skb->data, to_copy);
    
    // In Netlink, we don't usually keep leftovers if they don't fit,
    // but a real implementation would check for MSG_TRUNC.
    
    ssize_t ret = (ssize_t)to_copy;
    free_skb(skb);
    return ret;
}

static int netlink_poll(socket_t *sock, int events) {
    if (!sock || !sock->sk) return 0;
    netlink_sock_t *nsk = (netlink_sock_t *)sock->sk;
    int revents = 0;

    spinlock_acquire(&nsk->recv_queue.lock);
    if (!skb_queue_empty(&nsk->recv_queue)) {
        revents |= EPOLLIN;
    }
    spinlock_release(&nsk->recv_queue.lock);

    if (events & EPOLLOUT) {
        revents |= EPOLLOUT;
    }

    return revents;
}

static void netlink_destroy(socket_t *sock) {
    if (sock->sk) {
        kfree(sock->sk);
        sock->sk = NULL;
    }
}

static sock_ops_t netlink_ops = {
    .bind = netlink_bind,
    .recv = netlink_recv,
    .poll = netlink_poll,
    .setsockopt = netlink_setsockopt,
    .getsockopt = netlink_getsockopt,
    .getsockname = netlink_getsockname,
    .destroy = netlink_destroy,
};

int netlink_create(socket_t *sock, int protocol) {
    netlink_sock_t *nsk = kmalloc(sizeof(netlink_sock_t));
    if (!nsk) return -12; // ENOMEM

    memset(nsk, 0, sizeof(netlink_sock_t));
    nsk->protocol = protocol;
    spinlock_init(&nsk->recv_queue.lock);
    nsk->recv_queue.head = NULL;
    nsk->recv_queue.tail = NULL;
    nsk->recv_queue.len = 0;

    sock->sk = nsk;
    sock->ops = &netlink_ops;
    sock->state = SS_UNCONNECTED;

    klog_puts("[OK] AF_NETLINK socket created (protocol=");
    klog_uint64((uint64_t)protocol);
    klog_puts(")\n");

    return 0;
}

static net_family_t netlink_family = {
    .family = AF_NETLINK,
    .create = netlink_create,
    .next = NULL
};

void af_netlink_init(void) {
    sock_register_family(&netlink_family);
    klog_puts("[OK] AF_NETLINK protocol initialized\n");
}
