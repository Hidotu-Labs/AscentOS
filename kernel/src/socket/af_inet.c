#include "af_inet.h"
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../net/byteorder.h"
#include "../net/net.h"
#include "../net/netif.h"
#include "../net/tcp.h"
#include "../net/udp.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "socket_internal.h"

// ---------------------------------------------------------------------------
// Network interface ioctl constants (from linux/sockios.h)
// ---------------------------------------------------------------------------
#define SIOCGIFCONF     0x8912  // Get interface list
#define SIOCGIFFLAGS    0x8913  // Get interface flags
#define SIOCSIFFLAGS    0x8914  // Set interface flags
#define SIOCGIFADDR     0x8915  // Get interface address
#define SIOCGIFDSTADDR  0x8917  // Get destination address
#define SIOCGIFBRDADDR  0x8919  // Get broadcast address
#define SIOCGIFNETMASK  0x891b  // Get netmask
#define SIOCGIFMTU      0x8921  // Get MTU
#define SIOCGIFHWADDR   0x8927  // Get hardware address
#define SIOCGIFINDEX    0x8933  // Get interface index

// Interface flags
#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
#define IFF_MULTICAST 0x1000

#define IFNAMSIZ 16

struct ifreq_addr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct ifreq {
    union {
        char ifrn_name[IFNAMSIZ];
    } ifr_ifrn;
    union {
        struct ifreq_addr ifru_addr;
        struct ifreq_addr ifru_dstaddr;
        struct ifreq_addr ifru_broadaddr;
        struct ifreq_addr ifru_netmask;
        struct ifreq_addr ifru_hwaddr;
        short             ifru_flags;
        int               ifru_ivalue;
        int               ifru_mtu;
        char              ifru_slave[IFNAMSIZ];
        char              ifru_newname[IFNAMSIZ];
    } ifr_ifru;
};

#define ifr_name    ifr_ifrn.ifrn_name
#define ifr_flags   ifr_ifru.ifru_flags
#define ifr_addr    ifr_ifru.ifru_addr
#define ifr_dstaddr ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_netmask ifr_ifru.ifru_netmask
#define ifr_hwaddr  ifr_ifru.ifru_hwaddr
#define ifr_mtu     ifr_ifru.ifru_mtu
#define ifr_ifindex ifr_ifru.ifru_ivalue

struct ifconf {
    int   ifc_len;
    union {
        char       *ifc_buf;
        struct ifreq *ifc_req;
    } ifc_ifcu;
};

#define ifc_buf ifc_ifcu.ifc_buf
#define ifc_req ifc_ifcu.ifc_req

// ---------------------------------------------------------------------------
// Helper: fill a sockaddr with an IPv4 address
// ---------------------------------------------------------------------------
static void fill_sockaddr_in(struct ifreq_addr *sa, uint32_t ip_host_order) {
    memset(sa, 0, sizeof(*sa));
    sa->sa_family = 2; // AF_INET
    // sa_data: port(2) + addr(4) in network byte order
    uint32_t ip_net = htonl(ip_host_order);
    sa->sa_data[0] = 0; // port hi
    sa->sa_data[1] = 0; // port lo
    sa->sa_data[2] = (char)((ip_net >> 24) & 0xFF);
    sa->sa_data[3] = (char)((ip_net >> 16) & 0xFF);
    sa->sa_data[4] = (char)((ip_net >>  8) & 0xFF);
    sa->sa_data[5] = (char)( ip_net        & 0xFF);
}

// ---------------------------------------------------------------------------
// AF_INET ioctl — handles network interface query ioctls used by glibc/GIO
// ---------------------------------------------------------------------------
static int inet_ioctl(socket_t *sock, uint32_t request, uint64_t arg) {
    (void)sock;

    netif_t *nif = netif_get();

    switch (request) {
    case SIOCGIFCONF: {
        // Returns list of configured interfaces.
        // GIO uses this to enumerate network interfaces.
        struct ifconf *ifc = (struct ifconf *)arg;
        if (!ifc)
            return -22; // EINVAL

        struct ifreq *req = (struct ifreq *)(uintptr_t)ifc->ifc_buf;
        int max_count = ifc->ifc_len / (int)sizeof(struct ifreq);

        if (!req || max_count <= 0) {
            // Just report needed buffer size
            ifc->ifc_len = 2 * (int)sizeof(struct ifreq); // lo + eth0
            return 0;
        }

        int count = 0;

        // Loopback interface
        if (count < max_count) {
            memset(&req[count], 0, sizeof(struct ifreq));
            strcpy(req[count].ifr_name, "lo");
            fill_sockaddr_in(&req[count].ifr_addr, 0x7F000001); // 127.0.0.1
            count++;
        }

        // Physical interface (if configured)
        if (count < max_count && nif && nif->up && nif->ip != 0) {
            memset(&req[count], 0, sizeof(struct ifreq));
            strcpy(req[count].ifr_name, "eth0");
            fill_sockaddr_in(&req[count].ifr_addr, nif->ip);
            count++;
        }

        ifc->ifc_len = count * (int)sizeof(struct ifreq);
        return 0;
    }

    case SIOCGIFFLAGS: {
        struct ifreq *ifr = (struct ifreq *)arg;
        if (!ifr)
            return -22;

        if (strcmp(ifr->ifr_name, "lo") == 0) {
            ifr->ifr_flags = (short)(IFF_UP | IFF_LOOPBACK | IFF_RUNNING);
        } else if (strcmp(ifr->ifr_name, "eth0") == 0) {
            if (nif && nif->up)
                ifr->ifr_flags = (short)(IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST);
            else
                ifr->ifr_flags = 0;
        } else {
            return -19; // ENODEV
        }
        return 0;
    }

    case SIOCGIFADDR: {
        struct ifreq *ifr = (struct ifreq *)arg;
        if (!ifr)
            return -22;

        if (strcmp(ifr->ifr_name, "lo") == 0) {
            fill_sockaddr_in(&ifr->ifr_addr, 0x7F000001); // 127.0.0.1
        } else if (strcmp(ifr->ifr_name, "eth0") == 0) {
            uint32_t ip = (nif && nif->up) ? nif->ip : 0;
            fill_sockaddr_in(&ifr->ifr_addr, ip);
        } else {
            return -19; // ENODEV
        }
        return 0;
    }

    case SIOCGIFNETMASK: {
        struct ifreq *ifr = (struct ifreq *)arg;
        if (!ifr)
            return -22;

        if (strcmp(ifr->ifr_name, "lo") == 0) {
            fill_sockaddr_in(&ifr->ifr_netmask, 0xFF000000); // 255.0.0.0
        } else if (strcmp(ifr->ifr_name, "eth0") == 0) {
            uint32_t mask = (nif && nif->up) ? nif->netmask : 0xFFFFFF00;
            fill_sockaddr_in(&ifr->ifr_netmask, mask);
        } else {
            return -19; // ENODEV
        }
        return 0;
    }

    case SIOCGIFBRDADDR: {
        struct ifreq *ifr = (struct ifreq *)arg;
        if (!ifr)
            return -22;

        if (strcmp(ifr->ifr_name, "lo") == 0) {
            fill_sockaddr_in(&ifr->ifr_broadaddr, 0x7FFFFFFF); // 127.255.255.255
        } else if (strcmp(ifr->ifr_name, "eth0") == 0) {
            uint32_t bcast = 0;
            if (nif && nif->up)
                bcast = (nif->ip & nif->netmask) | (~nif->netmask);
            fill_sockaddr_in(&ifr->ifr_broadaddr, bcast);
        } else {
            return -19; // ENODEV
        }
        return 0;
    }

    case SIOCGIFMTU: {
        struct ifreq *ifr = (struct ifreq *)arg;
        if (!ifr)
            return -22;

        if (strcmp(ifr->ifr_name, "lo") == 0)
            ifr->ifr_mtu = 65536;
        else if (strcmp(ifr->ifr_name, "eth0") == 0)
            ifr->ifr_mtu = 1500;
        else
            return -19; // ENODEV
        return 0;
    }

    case SIOCGIFHWADDR: {
        struct ifreq *ifr = (struct ifreq *)arg;
        if (!ifr)
            return -22;

        memset(&ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
        ifr->ifr_hwaddr.sa_family = 1; // ARPHRD_ETHER

        if (strcmp(ifr->ifr_name, "lo") == 0) {
            // loopback has all-zero MAC
        } else if (strcmp(ifr->ifr_name, "eth0") == 0) {
            if (nif)
                memcpy(ifr->ifr_hwaddr.sa_data, nif->mac, 6);
        } else {
            return -19; // ENODEV
        }
        return 0;
    }

    case SIOCGIFINDEX: {
        struct ifreq *ifr = (struct ifreq *)arg;
        if (!ifr)
            return -22;

        if (strcmp(ifr->ifr_name, "lo") == 0)
            ifr->ifr_ifindex = 1;
        else if (strcmp(ifr->ifr_name, "eth0") == 0)
            ifr->ifr_ifindex = 2;
        else
            return -19; // ENODEV
        return 0;
    }

    case SIOCSIFFLAGS:
        // Silently accept flag changes (we don't actually configure anything)
        return 0;

    default:
        klog_puts("[INET_IOCTL] unhandled request=0x");
        klog_hex32(request);
        klog_puts("\n");
        return -25; // ENOTTY
    }
}

// TCP ↔ AF_INET Mapping
static inet_sock_t *tcp_inet_map[MAX_TCP_SOCKETS];

// Early Data Buffer
// On loopback, data can arrive at a child TCP socket before inet_accept()
// has set up the tcp_inet_map entry. We buffer it here and drain in accept.
static sk_buff_t *early_data[MAX_TCP_SOCKETS];

// UDP ↔ AF_INET Mapping
static inet_sock_t *udp_inet_map[MAX_UDP_SOCKETS];

// AF_INET Family Registration
int inet_create(socket_t *sock, int protocol);

static net_family_t inet_family_ops = {
    .family = AF_INET,
    .create = inet_create,
};

// TCP Data Bridge Callback
static void tcp_data_callback(int sock_id, const uint8_t *data, uint16_t len) {
  if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS)
    return;
  inet_sock_t *inet = tcp_inet_map[sock_id];
  if (!inet) {
    if (!data || len == 0)
      return; // Don't buffer early FINs
    // Loopback: data arrived before inet_accept set up the mapping.
    // Chain it in the early_data list for this socket.
    sk_buff_t *skb = alloc_skb(len);
    if (!skb)
      return;
    memcpy(skb->data, data, len);
    skb->len = len;
    skb->next = NULL;
    if (!early_data[sock_id]) {
      early_data[sock_id] = skb;
    } else {
      sk_buff_t *tail = early_data[sock_id];
      while (tail->next)
        tail = tail->next;
      tail->next = skb;
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
    // Peer sent FIN
    if (inet->parent)
      inet->parent->state = SS_UNCONNECTED;
  }

  if (inet->parent)
    socket_wake(inet->parent);
}

static void tcp_event_callback(int sock_id) {
  if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS)
    return;
  inet_sock_t *inet = tcp_inet_map[sock_id];
  if (inet && inet->parent) {
    socket_wake(inet->parent);
  }
}

// UDP Data Bridge Callback
static void udp_data_callback(uint16_t local_port, const uint8_t *data,
                              uint16_t len, uint32_t src_ip,
                              uint16_t src_port) {
  inet_sock_t *inet = NULL;
  for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
    if (udp_inet_map[i] && udp_inet_map[i]->udp_port == local_port) {
      inet = udp_inet_map[i];
      break;
    }
  }

  if (!inet) {
    klog_puts("[UDP_CB] no inet_sock for port=");
    klog_uint64(local_port);
    klog_puts("\n");
    return;
  }

  klog_puts("[UDP_CB] queuing ");
  klog_uint64(len);
  klog_puts(" bytes for port=");
  klog_uint64(local_port);
  klog_puts("\n");

  sk_buff_t *skb = alloc_skb(len);
  if (skb) {
    memcpy(skb->data, data, len);
    skb->len = len;
    skb->src_ip = src_ip;
    skb->src_port = src_port;
    skb_queue_tail(&inet->receive_queue, skb);
  }

  if (inet->parent)
    socket_wake(inet->parent);
}

// AF_INET Socket Operations
static sock_ops_t inet_stream_ops = {
    .bind = inet_bind,
    .connect = inet_connect,
    .send = inet_send,
    .recv = inet_recv,
    .listen = inet_listen,
    .accept = inet_accept,
    .sendto = inet_sendto,
    .recvfrom = inet_recvfrom,
    .getsockopt = NULL,
    .setsockopt = NULL,
    .shutdown = NULL,
    .poll = inet_poll,
    .ioctl = inet_ioctl,
    .getsockname = inet_getsockname,
    .getpeername = inet_getpeername,
    .destroy = inet_destroy,
};

static sock_ops_t inet_dgram_ops = {
    .bind = inet_bind,
    .connect = inet_connect,
    .send = inet_send,
    .recv = inet_recv,
    .listen = NULL,
    .accept = NULL,
    .sendto = inet_sendto,
    .recvfrom = inet_recvfrom,
    .getsockopt = NULL,
    .setsockopt = NULL,
    .shutdown = NULL,
    .poll = inet_poll,
    .ioctl = inet_ioctl,
    .getsockname = inet_getsockname,
    .getpeername = inet_getpeername,
    .destroy = inet_destroy,
};

int inet_create(socket_t *sock, int protocol) {
  (void)protocol;

  inet_sock_t *inet = kmalloc(sizeof(inet_sock_t));
  if (!inet)
    return -12; // ENOMEM

  memset(inet, 0, sizeof(inet_sock_t));
  inet->parent = sock;
  inet->tcp_sock_id = -1;

  inet->receive_queue.head = NULL;
  inet->receive_queue.tail = NULL;
  inet->receive_queue.len = 0;
  spinlock_init(&inet->receive_queue.lock);

  sock->sk = inet;

  if (sock->type == SOCK_STREAM) {
    sock->ops = &inet_stream_ops;
  } else if (sock->type == SOCK_DGRAM) {
    sock->ops = &inet_dgram_ops;
  } else {
    kfree(inet);
    return -93; // EPROTONOSUPPORT
  }

  return 0;
}

int inet_bind(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (addrlen < (int)sizeof(struct sockaddr_in))
    return -22; // EINVAL

  struct sockaddr_in *sin = (struct sockaddr_in *)addr;
  inet_sock_t *inet = (inet_sock_t *)sock->sk;
  memcpy(&inet->local_addr, sin, sizeof(struct sockaddr_in));

  /* Fill in local IP from netif if not specified */
  if (inet->local_addr.sin_addr.s_addr == 0) {
    netif_t *nif = netif_get();
    if (nif)
      inet->local_addr.sin_addr.s_addr = htonl(nif->ip);
  }

  if (sock->type == SOCK_DGRAM) {
    uint16_t port = ntohs(sin->sin_port);
    int bound_port = udp_bind(port, udp_data_callback);
    if (bound_port < 0)
      return -98; // EADDRINUSE

    inet->udp_port = (uint16_t)bound_port;
    inet->local_addr.sin_port = htons(inet->udp_port);

    klog_puts("[INET_BIND] UDP assigned port=");
    klog_uint64(inet->udp_port);
    klog_puts("\n");

    // Register in map
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
      if (!udp_inet_map[i]) {
        udp_inet_map[i] = inet;
        klog_puts("[INET_BIND] registered in udp_inet_map[");
        klog_uint64(i);
        klog_puts("]\n");
        break;
      }
    }
  }

  return 0;
}

int inet_connect(socket_t *sock, struct sockaddr *addr, int addrlen) {
  if (addrlen < (int)sizeof(struct sockaddr_in))
    return -22; // EINVAL

  struct sockaddr_in *sin = (struct sockaddr_in *)addr;
  inet_sock_t *inet = (inet_sock_t *)sock->sk;
  memcpy(&inet->remote_addr, sin, sizeof(struct sockaddr_in));

  uint32_t ip = ntohl(sin->sin_addr.s_addr);
  uint16_t port = ntohs(sin->sin_port);

  /* UDP connect: just record the remote address, no handshake. */
  if (sock->type == SOCK_DGRAM) {
    inet->remote_addr = *sin;
    /* Auto-bind to an ephemeral port if not already bound */
    if (inet->udp_port == 0) {
      int bound = udp_bind(0, udp_data_callback);
      if (bound < 0)
        return -98; // EADDRINUSE
      inet->udp_port = (uint16_t)bound;
      inet->local_addr.sin_port = htons(inet->udp_port);
      for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (!udp_inet_map[i]) {
          udp_inet_map[i] = inet;
          break;
        }
      }
    }
    sock->state = SS_CONNECTED;
    return 0;
  }

  sock->state = SS_CONNECTING;

  int sock_id = tcp_connect(ip, port, tcp_data_callback);
  if (sock_id < 0) {
    sock->state = SS_UNCONNECTED;
    // tcp_connect returns -ECONNREFUSED (-111) on RST, -ETIMEDOUT (-110) on
    // timeout
    return sock_id;
  }

  inet->tcp_sock_id = sock_id;
  tcp_inet_map[sock_id] = inet;

  // Set callbacks to bridge TCP events to socket wait queues
  tcp_set_callbacks(sock_id, tcp_data_callback, tcp_event_callback);

  // Manual driving of loopback synchronously if needed
  net_poll();

  sock->state = SS_CONNECTED;
  return 0;
}

int inet_listen(socket_t *sock, int backlog) {
  (void)backlog;
  inet_sock_t *inet = (inet_sock_t *)sock->sk;

  uint16_t port = ntohs(inet->local_addr.sin_port);
  if (port == 0)
    return -22;

  int sock_id = tcp_listen(port, tcp_data_callback);
  if (sock_id < 0)
    return -98;

  inet->tcp_sock_id = sock_id;
  tcp_inet_map[sock_id] = inet;

  // Set callbacks to bridge TCP events to socket wait queues
  tcp_set_callbacks(sock_id, tcp_data_callback, tcp_event_callback);

  sock->state = SS_LISTENING;
  return 0;
}

int inet_accept(socket_t *sock, socket_t **newsock) {
  inet_sock_t *inet = (inet_sock_t *)sock->sk;
  if (inet->tcp_sock_id < 0)
    return -22;

  int new_tcp_id = -1;

  while ((new_tcp_id = tcp_accept(inet->tcp_sock_id)) < 0) {
    if (sock->flags & SOCK_NONBLOCK)
      return -11;

    // Drain the full RX queue — required since there are no RX interrupts
    while (net_poll())
      ;
    sched_yield();
  }

  socket_t *new_sock = socket_create(AF_INET, SOCK_STREAM, 0);
  if (!new_sock)
    return -12;

  inet_sock_t *new_inet = (inet_sock_t *)new_sock->sk;
  new_inet->tcp_sock_id = new_tcp_id;
  tcp_inet_map[new_tcp_id] = new_inet;

  // Set callbacks to bridge TCP events to socket wait queues
  tcp_set_callbacks(new_tcp_id, tcp_data_callback, tcp_event_callback);

  // Drain any early data that arrived via loopback before the mapping existed
  while (early_data[new_tcp_id]) {
    sk_buff_t *skb = early_data[new_tcp_id];
    early_data[new_tcp_id] = skb->next;
    skb->next = NULL;
    skb_queue_tail(&new_inet->receive_queue, skb);
  }

  uint32_t remote_ip;
  uint16_t remote_port;
  tcp_get_remote_info(new_tcp_id, &remote_ip, &remote_port);
  new_inet->remote_addr.sin_family = AF_INET;
  new_inet->remote_addr.sin_port = htons(remote_port);
  new_inet->remote_addr.sin_addr.s_addr = htonl(remote_ip);

  new_sock->state = SS_CONNECTED;
  *newsock = new_sock;
  return 0;
}

int inet_poll(socket_t *sock, int events) {
  inet_sock_t *inet = (inet_sock_t *)sock->sk;
  int revents = 0;

  if (events & POLLIN) {
    if (!skb_queue_empty(&inet->receive_queue))
      revents |= POLLIN;
  }

  if (events & POLLOUT) {
    if (sock->type == SOCK_DGRAM) {
      /* UDP sockets are always writable (sendto doesn't require SS_CONNECTED)
       */
      revents |= POLLOUT;
    } else if (sock->state == SS_CONNECTED) {
      revents |= POLLOUT;
    }
  }

  if (sock->type == SOCK_STREAM && sock->state == SS_UNCONNECTED &&
      inet->tcp_sock_id != -1) {
    revents |= POLLHUP;
  }

  return revents;
}

ssize_t inet_send(socket_t *sock, const void *buf, size_t len, int flags) {
  (void)flags;
  inet_sock_t *inet = (inet_sock_t *)sock->sk;

  if (inet->tcp_sock_id < 0)
    return -107;

  size_t total_sent = 0;
  const uint8_t *ptr = (const uint8_t *)buf;

  while (total_sent < len) {
    uint16_t chunk =
        (len - total_sent) > 1400 ? 1400 : (uint16_t)(len - total_sent);
    int ret = tcp_send(inet->tcp_sock_id, ptr + total_sent, chunk);
    if (ret < 0)
      return total_sent > 0 ? (ssize_t)total_sent : -104;
    total_sent += ret;
  }

  return (ssize_t)total_sent;
}

ssize_t inet_recv(socket_t *sock, void *buf, size_t len, int flags) {
  return inet_recvfrom(sock, buf, len, flags, NULL, NULL);
}

ssize_t inet_sendto(socket_t *sock, const void *buf, size_t len, int flags,
                    struct sockaddr *dest_addr, int addrlen) {
  (void)flags;
  inet_sock_t *inet = (inet_sock_t *)sock->sk;

  if (sock->type == SOCK_STREAM)
    return inet_send(sock, buf, len, flags);

  uint32_t dest_ip;
  uint16_t dest_port;

  if (!dest_addr || addrlen < (int)sizeof(struct sockaddr_in)) {
    /* No destination given — use connected address (POSIX: sendto on a
     * connected UDP socket with NULL dest is equivalent to send). */
    if (sock->state != SS_CONNECTED)
      return -22; // EINVAL: not connected and no dest given
    dest_ip = ntohl(inet->remote_addr.sin_addr.s_addr);
    dest_port = ntohs(inet->remote_addr.sin_port);
  } else {
    struct sockaddr_in *sin = (struct sockaddr_in *)dest_addr;
    dest_ip = ntohl(sin->sin_addr.s_addr);
    dest_port = ntohs(sin->sin_port);
  }

  // If not bound, bind to an ephemeral port
  if (inet->udp_port == 0) {
    int port = udp_bind(0, udp_data_callback);
    if (port < 0)
      return -105; // ENOBUFS
    inet->udp_port = (uint16_t)port;
    inet->local_addr.sin_port = htons(inet->udp_port);

    // Register in map
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
      if (!udp_inet_map[i]) {
        udp_inet_map[i] = inet;
        break;
      }
    }
  }

  int ret = udp_send_packet(dest_ip, inet->udp_port, dest_port, buf, len);
  if (ret < 0)
    return ret;

  return (ssize_t)len;
}

ssize_t inet_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
                      struct sockaddr *src_addr, int *addrlen) {
  inet_sock_t *inet = (inet_sock_t *)sock->sk;

  /* Non-blocking if socket is O_NONBLOCK or MSG_DONTWAIT was passed */
  bool nonblock = (sock->flags & SOCK_NONBLOCK) || (flags & MSG_DONTWAIT);

  while (skb_queue_empty(&inet->receive_queue)) {
    if (sock->type == SOCK_STREAM && sock->state != SS_CONNECTED &&
        sock->state != SS_CONNECTING)
      return 0;

    if (nonblock)
      return -11; /* EAGAIN */

    /* NIC is poll-driven: drain all pending RX frames before re-checking
     * the receive queue, so multi-segment TLS records are fully assembled. */
    while (net_poll())
      ;
    sched_yield();
  }

  sk_buff_t *skb = skb_dequeue(&inet->receive_queue);
  if (!skb)
    return 0;

  size_t copy_len = len < skb->len ? len : skb->len;
  memcpy(buf, skb->data, copy_len);

  if (src_addr && addrlen && *addrlen >= (int)sizeof(struct sockaddr_in)) {
    struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
    memset(sin, 0, sizeof(struct sockaddr_in));
    sin->sin_family = AF_INET;
    sin->sin_port = htons(skb->src_port);
    sin->sin_addr.s_addr = htonl(skb->src_ip);
    *addrlen = sizeof(struct sockaddr_in);
  }

  if (copy_len < skb->len && sock->type == SOCK_STREAM) {
    size_t remain = skb->len - copy_len;
    sk_buff_t *rest = alloc_skb(remain);
    if (rest) {
      memcpy(rest->data, skb->data + copy_len, remain);
      rest->len = remain;
      /* Put the remainder back at the HEAD so subsequent reads see
       * the correct byte stream order, not after later segments. */
      skb_queue_head(&inet->receive_queue, rest);
    }
  }

  free_skb(skb);
  return (ssize_t)copy_len;
}

int inet_getsockname(socket_t *sock, struct sockaddr *addr, int *addrlen) {
  if (!addr || !addrlen)
    return -22; // EINVAL
  inet_sock_t *inet = (inet_sock_t *)sock->sk;
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;

  if (sock->type == SOCK_DGRAM) {
    /* For UDP, report the bound port (may be ephemeral). */
    sin.sin_port = htons(inet->udp_port);
    sin.sin_addr.s_addr = inet->local_addr.sin_addr.s_addr;
  } else {
    sin.sin_port = inet->local_addr.sin_port;
    sin.sin_addr.s_addr = inet->local_addr.sin_addr.s_addr;
  }

  int copy = *addrlen < (int)sizeof(sin) ? *addrlen : (int)sizeof(sin);
  memcpy(addr, &sin, copy);
  *addrlen = sizeof(sin);
  return 0;
}

int inet_getpeername(socket_t *sock, struct sockaddr *addr, int *addrlen) {
  if (!addr || !addrlen)
    return -22; // EINVAL
  if (sock->state != SS_CONNECTED)
    return -107; // ENOTCONN
  inet_sock_t *inet = (inet_sock_t *)sock->sk;
  int copy = *addrlen < (int)sizeof(inet->remote_addr)
                 ? *addrlen
                 : (int)sizeof(inet->remote_addr);
  memcpy(addr, &inet->remote_addr, copy);
  *addrlen = sizeof(inet->remote_addr);
  return 0;
}

void inet_destroy(socket_t *sock) {
  inet_sock_t *inet = (inet_sock_t *)sock->sk;
  if (!inet)
    return;

  if (inet->tcp_sock_id >= 0 && inet->tcp_sock_id < MAX_TCP_SOCKETS) {
    // Free any unclaimed early data
    while (early_data[inet->tcp_sock_id]) {
      sk_buff_t *skb = early_data[inet->tcp_sock_id];
      early_data[inet->tcp_sock_id] = skb->next;
      free_skb(skb);
    }
    tcp_inet_map[inet->tcp_sock_id] = NULL;
    tcp_close(inet->tcp_sock_id);
    inet->tcp_sock_id = -1;
  }

  if (inet->udp_port > 0) {
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
      if (udp_inet_map[i] == inet) {
        udp_inet_map[i] = NULL;
        break;
      }
    }
    udp_unbind(inet->udp_port);
    inet->udp_port = 0;
  }

  sk_buff_t *skb;
  while ((skb = skb_dequeue(&inet->receive_queue)) != NULL)
    free_skb(skb);

  kfree(inet);
  sock->sk = NULL;
}

void af_inet_init(void) {
  memset(tcp_inet_map, 0, sizeof(tcp_inet_map));
  memset(udp_inet_map, 0, sizeof(udp_inet_map));
  memset(early_data, 0, sizeof(early_data));
  sock_register_family(&inet_family_ops);
  klog_puts("[OK] AF_INET protocol family registered\n");
}

#define STRESS_PORT 9999
#define STRESS_ITERATIONS 10
#define STRESS_MSG_SIZE 64

static volatile int stress_server_ready = 0;
static volatile bool stress_done = false;
static volatile int stress_errors = 0;
static volatile int stress_completed = 0;

static void stress_server_thread(void) {
  klog_puts("[STRESS] Server thread started\n");

  socket_t *server = socket_create(AF_INET, SOCK_STREAM, 0);
  if (!server) {
    klog_puts("[FAIL] Server: socket_create failed\n");
    __atomic_fetch_add(&stress_errors, 1, __ATOMIC_RELAXED);
    stress_done = true;
    return;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(STRESS_PORT);
  addr.sin_addr.s_addr = htonl(0x7F000001);

  if (socket_bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    klog_puts("[FAIL] Server: bind failed\n");
    __atomic_fetch_add(&stress_errors, 1, __ATOMIC_RELAXED);
    socket_put(server);
    stress_done = true;
    return;
  }

  if (socket_listen(server, 8) < 0) {
    klog_puts("[FAIL] Server: listen failed\n");
    __atomic_fetch_add(&stress_errors, 1, __ATOMIC_RELAXED);
    socket_put(server);
    stress_done = true;
    return;
  }

  klog_puts("[STRESS] Server listening on port 9999\n");
  __atomic_store_n(&stress_server_ready, 1, __ATOMIC_RELEASE);

  for (int i = 0; i < STRESS_ITERATIONS; i++) {
    socket_t *client = NULL;
    int ret = socket_accept(server, &client);
    if (ret < 0 || !client) {
      klog_puts("[FAIL] Server: accept failed\n");
      __atomic_fetch_add(&stress_errors, 1, __ATOMIC_RELAXED);
      continue;
    }

    char buf[STRESS_MSG_SIZE];
    ssize_t n = socket_recv(client, buf, sizeof(buf), 0);
    if (n > 0) {
      socket_send(client, buf, n, 0);
    }
    socket_put(client);
  }

  socket_put(server);
}

static void stress_client_thread(void) {
  klog_puts("[STRESS] Client thread started\n");

  while (!__atomic_load_n(&stress_server_ready, __ATOMIC_ACQUIRE)) {
    sched_yield();
  }
  klog_puts("[STRESS] Client starting iterations...\n");

  for (int i = 0; i < STRESS_ITERATIONS; i++) {
    socket_t *sock = socket_create(AF_INET, SOCK_STREAM, 0);
    if (!sock) {
      __atomic_fetch_add(&stress_errors, 1, __ATOMIC_RELAXED);
      continue;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(STRESS_PORT);
    addr.sin_addr.s_addr = htonl(0x7F000001);

    if (socket_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      klog_puts("[FAIL] Client: connect failed\n");
      __atomic_fetch_add(&stress_errors, 1, __ATOMIC_RELAXED);
      socket_put(sock);
      continue;
    }

    char send_buf[STRESS_MSG_SIZE];
    for (int j = 0; j < STRESS_MSG_SIZE; j++)
      send_buf[j] = (char)j;
    socket_send(sock, send_buf, STRESS_MSG_SIZE, 0);

    char recv_buf[STRESS_MSG_SIZE];
    ssize_t received = socket_recv(sock, recv_buf, STRESS_MSG_SIZE, 0);
    if (received != STRESS_MSG_SIZE) {
      klog_puts("E");
      __atomic_fetch_add(&stress_errors, 1, __ATOMIC_RELAXED);
    } else {
      __atomic_fetch_add(&stress_completed, 1, __ATOMIC_RELAXED);
    }

    socket_put(sock);
  }

  stress_done = true;
}

void af_inet_self_test(void) {
  klog_puts("[TEST] AF_INET Phase 2 Stress Test starting...\n");

  stress_server_ready = false;
  stress_done = false;
  stress_errors = 0;
  stress_completed = 0;

  sched_create_kernel_thread(stress_server_thread, cpu_get_bsp(), true);
  sched_create_kernel_thread(stress_client_thread, cpu_get_bsp(), true);

  // Wait for the stress test to complete (with timeout)
  uint64_t test_start = lapic_timer_get_ms();
  while (!stress_done) {
    if (lapic_timer_get_ms() - test_start > 30000) { // 30 second timeout
      klog_puts("\n[FAIL] AF_INET Phase 2 Stress Test TIMED OUT\n");
      return;
    }
    while (net_poll())
      ;
    sched_yield();
  }

  // Give server thread a moment to finish its last iteration
  for (int i = 0; i < 100; i++) {
    net_poll();
    sched_yield();
  }

  if (stress_errors == 0 && stress_completed == STRESS_ITERATIONS) {
    klog_puts("\n[OK] AF_INET Phase 2 Stress Test PASSED\n");
  } else {
    klog_puts("\n[FAIL] AF_INET Phase 2 Stress Test FAILED (errors=");
    klog_uint64(stress_errors);
    klog_puts(", completed=");
    klog_uint64(stress_completed);
    klog_puts("/");
    klog_uint64(STRESS_ITERATIONS);
    klog_puts(")\n");
  }
}