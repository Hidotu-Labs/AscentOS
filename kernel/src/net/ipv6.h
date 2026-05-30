#ifndef NET_IPV6_H
#define NET_IPV6_H

#include <stdint.h>
#include <stddef.h>

/* IPv6 next-header values (same numbers as IPv4 protocol field) */
#define IPV6_PROTO_TCP   6
#define IPV6_PROTO_UDP   17
#define IPV6_PROTO_ICMPV6 58

/* EtherType for IPv6 */
#define ETHERTYPE_IPV6 0x86DD

/* IPv6 header — 40 bytes, no checksum */
typedef struct __attribute__((packed)) {
    uint32_t ver_tc_fl;     /* version(4) | traffic-class(8) | flow-label(20) */
    uint16_t payload_len;   /* length of payload (after this header) */
    uint8_t  next_header;   /* next protocol (TCP=6, UDP=17, ICMPv6=58) */
    uint8_t  hop_limit;     /* TTL equivalent */
    uint8_t  src[16];       /* source address */
    uint8_t  dst[16];       /* destination address */
} ipv6_header_t;

/* Compute the internet checksum over a pseudo-header + payload for IPv6.
 * src and dst are 16-byte arrays in network byte order. */
uint16_t ipv6_checksum(const uint8_t *src, const uint8_t *dst,
                       uint8_t next_header, uint16_t payload_len,
                       const void *payload);

/* Handle an incoming IPv6 Ethernet payload */
void ipv6_handle_packet(const uint8_t *data, uint16_t len);

/* Send an IPv6 packet to dst (16-byte network-order address).
 * next_header is the protocol (TCP/UDP/ICMPv6).
 * Returns 0 on success, -1 on error. */
int ipv6_send_packet(const uint8_t *dst, uint8_t next_header,
                     const void *data, uint16_t len);

/* Initialise IPv6 — assigns a link-local address from the MAC */
void ipv6_init(void);

/* Get our link-local address (16 bytes, network order) */
const uint8_t *ipv6_get_linklocal(void);

/* Get our global unicast address if one was assigned via SLAAC (may be NULL) */
const uint8_t *ipv6_get_global(void);

/* Called from ethernet.c with the source MAC for NDP learning */
void ipv6_handle_packet_with_mac(const uint8_t *data, uint16_t len,
                                 const uint8_t *src_mac);

/* Bridge callbacks — implemented in af_inet6.c */
void tcp6_handle_packet(const uint8_t *payload, uint16_t len,
                        const uint8_t *src6, const uint8_t *dst6);
void udp6_handle_packet(const uint8_t *payload, uint16_t len,
                        const uint8_t *src6, const uint8_t *dst6);

/* Compare two 16-byte IPv6 addresses */
static inline int ipv6_addr_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static inline int ipv6_addr_is_zero(const uint8_t *a) {
    for (int i = 0; i < 16; i++)
        if (a[i]) return 0;
    return 1;
}

#endif /* NET_IPV6_H */
