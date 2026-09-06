#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int64_t ssize_t;

#define UDP_PORT_EPHEMERAL_MIN 49152
#define UDP_PORT_EPHEMERAL_MAX 65535
#define UDP_MAX_SOCKETS        64
#define UDP_RX_QUEUE_DEPTH     64
/* IPv4 fragmentation is not implemented: 1500 - 20 (IPv4) - 8 (UDP). */
#define UDP_PAYLOAD_MAX        1472

struct udp_socket;

struct udp_rxbuf {
    uint8_t  data[UDP_PAYLOAD_MAX];
    uint16_t length;
    uint32_t src_ip;
    uint16_t src_port;
};

struct udp_socket {
    bool     used;
    bool     bound;
    bool     connected;
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    struct udp_rxbuf queue[UDP_RX_QUEUE_DEPTH];
    uint32_t q_head;
    uint32_t q_tail;
    void    *wait_queue;
    void    *vfs_node;
    bool     nonblocking;
    int      rcvtimeo_ms;
    int      sndtimeo_ms;
};

void udp_init(void);

struct udp_socket *udp_socket_alloc(void);
void               udp_socket_free(struct udp_socket *s);

int  udp_bind(struct udp_socket *s, uint32_t ip, uint16_t port);
int  udp_connect(struct udp_socket *s, uint32_t ip, uint16_t port);
int  udp_disconnect(struct udp_socket *s);

ssize_t udp_sendto(struct udp_socket *s, const void *buf, size_t len,
                   uint32_t dst_ip, uint16_t dst_port);

ssize_t udp_recvfrom(struct udp_socket *s, void *buf, size_t len,
                     uint32_t *src_ip, uint16_t *src_port, bool nonblocking,
                     int timeout_ms);

void udp_deliver(uint32_t src_ip, uint16_t src_port,
                 uint16_t dst_port, const uint8_t *payload, uint16_t length);

bool net_phase6_init(void);

/* Snapshot of one UDP socket for /proc/net/udp */
struct udp_entry_snapshot {
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    bool     connected;
};

/* Fill up to 'max' entries; returns number filled. */
int udp_get_snapshot(struct udp_entry_snapshot *out, int max);

#endif
