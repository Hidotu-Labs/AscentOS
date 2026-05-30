/*
 * IPv6 — packet receive/transmit and link-local address management.
 *
 * We support:
 *   - Stateless link-local address (fe80::/10) derived from MAC (EUI-64)
 *   - Receiving TCP and UDP over IPv6
 *   - Sending TCP and UDP over IPv6 via the gateway MAC (QEMU SLIRP)
 *   - ICMPv6 Neighbor Solicitation / Advertisement (NDP) for link-local
 *   - Multicast listener join (silently accepted, no MLD sent)
 */

#include "net/ipv6.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "net/ethernet.h"
#include "net/net.h"
#include "net/netif.h"
#include "net/byteorder.h"
#include "net/arp.h"
#include "lib/string.h"
#include "console/klog.h"
#include "drivers/net/nic.h"

#include <stdbool.h>
#include <stdint.h>

/* ── Our addresses ─────────────────────────────────────────────────────────── */
static uint8_t g_linklocal[16];   /* fe80::MAC/EUI-64 */
static uint8_t g_global[16];      /* assigned via RA/SLAAC, or zero */
static bool    g_have_global = false;

/* ── NDP neighbour cache (simple, fixed-size) ──────────────────────────────── */
#define NDP_CACHE_SIZE 16
typedef struct {
    uint8_t  ip6[16];
    uint8_t  mac[6];
    bool     valid;
} ndp_entry_t;
static ndp_entry_t ndp_cache[NDP_CACHE_SIZE];

/* ── Helpers ───────────────────────────────────────────────────────────────── */

/* Internet checksum over an arbitrary byte stream (big-endian word pairs) */
uint16_t ipv6_checksum(const uint8_t *src, const uint8_t *dst,
                       uint8_t next_header, uint16_t payload_len,
                       const void *payload)
{
    uint32_t sum = 0;

    /* Pseudo-header: src(16) + dst(16) + payload_len(4) + zeros(3) + next(1) */
    for (int i = 0; i < 16; i += 2)
        sum += (uint16_t)((src[i] << 8) | src[i+1]);
    for (int i = 0; i < 16; i += 2)
        sum += (uint16_t)((dst[i] << 8) | dst[i+1]);
    sum += (uint16_t)(payload_len);          /* upper 16 bits are 0 for <=64KB */
    sum += (uint16_t)next_header;

    /* Payload */
    const uint8_t *p = (const uint8_t *)payload;
    uint16_t rem = payload_len;
    while (rem > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2; rem -= 2;
    }
    if (rem)
        sum += (uint16_t)(p[0] << 8);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Build EUI-64 link-local from MAC */
static void make_linklocal(const uint8_t *mac, uint8_t *out)
{
    out[0]  = 0xFE; out[1]  = 0x80;
    out[2]  = 0;    out[3]  = 0;
    out[4]  = 0;    out[5]  = 0;
    out[6]  = 0;    out[7]  = 0;
    /* EUI-64: insert 0xFF 0xFE in the middle, flip U/L bit */
    out[8]  = mac[0] ^ 0x02;
    out[9]  = mac[1];
    out[10] = mac[2];
    out[11] = 0xFF;
    out[12] = 0xFE;
    out[13] = mac[3];
    out[14] = mac[4];
    out[15] = mac[5];
}

/* Solicited-node multicast: ff02::1:ffXX:XXXX */
static void solicited_node_mc(const uint8_t *ip6, uint8_t *mc)
{
    mc[0]  = 0xFF; mc[1]  = 0x02;
    mc[2]  = 0;    mc[3]  = 0;
    mc[4]  = 0;    mc[5]  = 0;
    mc[6]  = 0;    mc[7]  = 0;
    mc[8]  = 0;    mc[9]  = 0;
    mc[10] = 0;    mc[11] = 0x01;
    mc[12] = 0xFF;
    mc[13] = ip6[13];
    mc[14] = ip6[14];
    mc[15] = ip6[15];
}

/* Multicast MAC from IPv6 multicast address: 33:33:XX:XX:XX:XX */
static void mc_mac(const uint8_t *ip6, uint8_t *mac)
{
    mac[0] = 0x33; mac[1] = 0x33;
    mac[2] = ip6[12]; mac[3] = ip6[13];
    mac[4] = ip6[14]; mac[5] = ip6[15];
}

/* ── NDP cache ─────────────────────────────────────────────────────────────── */

static void ndp_learn(const uint8_t *ip6, const uint8_t *mac)
{
    /* Update existing */
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (ndp_cache[i].valid && ipv6_addr_eq(ndp_cache[i].ip6, ip6)) {
            memcpy(ndp_cache[i].mac, mac, 6);
            return;
        }
    }
    /* Insert new */
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (!ndp_cache[i].valid) {
            memcpy(ndp_cache[i].ip6, ip6, 16);
            memcpy(ndp_cache[i].mac, mac, 6);
            ndp_cache[i].valid = true;
            return;
        }
    }
    /* Evict slot 0 (simple) */
    memcpy(ndp_cache[0].ip6, ip6, 16);
    memcpy(ndp_cache[0].mac, mac, 6);
    ndp_cache[0].valid = true;
}

static const uint8_t *ndp_lookup(const uint8_t *ip6)
{
    for (int i = 0; i < NDP_CACHE_SIZE; i++)
        if (ndp_cache[i].valid && ipv6_addr_eq(ndp_cache[i].ip6, ip6))
            return ndp_cache[i].mac;
    return NULL;
}

/* ── ICMPv6 ────────────────────────────────────────────────────────────────── */

#define ICMPV6_ECHO_REQUEST   128
#define ICMPV6_ECHO_REPLY     129
#define ICMPV6_RS             133
#define ICMPV6_RA             134
#define ICMPV6_NS             135   /* Neighbor Solicitation */
#define ICMPV6_NA             136   /* Neighbor Advertisement */

/* ICMPv6 option types */
#define NDP_OPT_SRC_LL  1
#define NDP_OPT_TGT_LL  2

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
} icmpv6_hdr_t;

/* Send a Neighbor Advertisement for our address */
static void send_na(const uint8_t *tgt_ip6, const uint8_t *dst_ip6,
                    const uint8_t *dst_mac)
{
    netif_t *nif = netif_get();

    /* NA body: flags(4) + reserved(12) + target(16) + option(8) = 28 bytes */
    uint8_t body[28];
    memset(body, 0, sizeof(body));
    body[0] = 0x60; /* S=1 (solicited), O=1 (override) */
    memcpy(body + 4, tgt_ip6, 16);
    body[20] = NDP_OPT_TGT_LL;
    body[21] = 1; /* length in units of 8 bytes */
    memcpy(body + 22, nif->mac, 6);

    /* ICMPv6 header */
    uint8_t pkt[4 + 28];
    pkt[0] = ICMPV6_NA;
    pkt[1] = 0;
    pkt[2] = 0; pkt[3] = 0; /* checksum placeholder */
    memcpy(pkt + 4, body, 28);

    uint16_t csum = ipv6_checksum(tgt_ip6, dst_ip6, IPV6_PROTO_ICMPV6,
                                  sizeof(pkt), pkt);
    pkt[2] = csum >> 8;
    pkt[3] = csum & 0xFF;

    /* Build IPv6 header */
    ipv6_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.ver_tc_fl   = htonl(0x60000000u);
    hdr.payload_len = htons((uint16_t)sizeof(pkt));
    hdr.next_header = IPV6_PROTO_ICMPV6;
    hdr.hop_limit   = 255;
    memcpy(hdr.src, tgt_ip6, 16);
    memcpy(hdr.dst, dst_ip6, 16);

    uint8_t frame[sizeof(ipv6_header_t) + sizeof(pkt)];
    memcpy(frame, &hdr, sizeof(ipv6_header_t));
    memcpy(frame + sizeof(ipv6_header_t), pkt, sizeof(pkt));

    eth_send_frame(dst_mac, ETHERTYPE_IPV6, frame, sizeof(frame));
}

/* Send a Neighbor Solicitation for target_ip6 */
static void send_ns(const uint8_t *target_ip6)
{
    netif_t *nif = netif_get();

    /* Solicited-node multicast destination */
    uint8_t dst[16];
    solicited_node_mc(target_ip6, dst);
    uint8_t dst_mac[6];
    mc_mac(dst, dst_mac);

    /* NS body: reserved(4) + target(16) + option(8) = 28 bytes */
    uint8_t body[28];
    memset(body, 0, sizeof(body));
    memcpy(body + 4, target_ip6, 16);
    body[20] = NDP_OPT_SRC_LL;
    body[21] = 1;
    memcpy(body + 22, nif->mac, 6);

    uint8_t pkt[4 + 28];
    pkt[0] = ICMPV6_NS;
    pkt[1] = 0;
    pkt[2] = 0; pkt[3] = 0;
    memcpy(pkt + 4, body, 28);

    uint16_t csum = ipv6_checksum(g_linklocal, dst, IPV6_PROTO_ICMPV6,
                                  sizeof(pkt), pkt);
    pkt[2] = csum >> 8;
    pkt[3] = csum & 0xFF;

    ipv6_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.ver_tc_fl   = htonl(0x60000000u);
    hdr.payload_len = htons((uint16_t)sizeof(pkt));
    hdr.next_header = IPV6_PROTO_ICMPV6;
    hdr.hop_limit   = 255;
    memcpy(hdr.src, g_linklocal, 16);
    memcpy(hdr.dst, dst, 16);

    uint8_t frame[sizeof(ipv6_header_t) + sizeof(pkt)];
    memcpy(frame, &hdr, sizeof(ipv6_header_t));
    memcpy(frame + sizeof(ipv6_header_t), pkt, sizeof(pkt));

    eth_send_frame(dst_mac, ETHERTYPE_IPV6, frame, sizeof(frame));
}

/* Handle ICMPv6 */
static void icmpv6_handle(const uint8_t *payload, uint16_t len,
                          const uint8_t *src, const uint8_t *dst,
                          const uint8_t *src_mac)
{
    if (len < 4) return;
    const icmpv6_hdr_t *hdr = (const icmpv6_hdr_t *)payload;

    switch (hdr->type) {
    case ICMPV6_NS: {
        /* Neighbor Solicitation — someone wants our MAC */
        if (len < 4 + 4 + 16) break;
        const uint8_t *target = payload + 8; /* after type/code/csum/reserved */
        if (ipv6_addr_eq(target, g_linklocal) ||
            (g_have_global && ipv6_addr_eq(target, g_global))) {
            /* Learn the solicitor */
            if (src_mac)
                ndp_learn(src, src_mac);
            /* Reply with NA */
            send_na(target, src, src_mac ? src_mac : (const uint8_t *)"\x33\x33\x00\x00\x00\x01");
        }
        break;
    }
    case ICMPV6_NA: {
        /* Neighbor Advertisement — learn the MAC */
        if (len < 4 + 4 + 16) break;
        const uint8_t *target = payload + 8;
        /* Look for target link-layer option */
        const uint8_t *opt = payload + 24;
        uint16_t opt_rem = (uint16_t)(len - 24);
        while (opt_rem >= 8) {
            uint8_t otype = opt[0];
            uint8_t olen  = opt[1]; /* in units of 8 bytes */
            if (olen == 0) break;
            if (otype == NDP_OPT_TGT_LL && olen == 1) {
                ndp_learn(target, opt + 2);
                break;
            }
            opt     += olen * 8;
            opt_rem -= olen * 8;
        }
        /* Also learn from source if src_mac provided */
        if (src_mac && !ipv6_addr_is_zero(src))
            ndp_learn(src, src_mac);
        break;
    }
    case ICMPV6_RA: {
        /* Router Advertisement — extract prefix for SLAAC */
        if (len < 16) break;
        /* Learn router MAC */
        if (src_mac)
            ndp_learn(src, src_mac);
        /* Walk options looking for Prefix Information (type=3) */
        const uint8_t *opt = payload + 16;
        uint16_t opt_rem = (uint16_t)(len - 16);
        while (opt_rem >= 8) {
            uint8_t otype = opt[0];
            uint8_t olen  = opt[1];
            if (olen == 0) break;
            if (otype == 3 && olen == 4 && opt_rem >= 32) {
                /* Prefix Information option */
                uint8_t prefix_len = opt[2];
                uint8_t flags      = opt[3];
                /* A-flag (bit 6) = autonomous address config */
                if ((flags & 0x40) && prefix_len == 64 && !g_have_global) {
                    netif_t *nif = netif_get();
                    memcpy(g_global, opt + 16, 8);   /* prefix */
                    /* EUI-64 interface ID */
                    g_global[8]  = nif->mac[0] ^ 0x02;
                    g_global[9]  = nif->mac[1];
                    g_global[10] = nif->mac[2];
                    g_global[11] = 0xFF;
                    g_global[12] = 0xFE;
                    g_global[13] = nif->mac[3];
                    g_global[14] = nif->mac[4];
                    g_global[15] = nif->mac[5];
                    g_have_global = true;
                    klog_puts("[IPV6] SLAAC global address assigned\n");
                }
            }
            opt     += olen * 8;
            opt_rem -= (uint16_t)(olen * 8);
        }
        break;
    }
    default:
        break;
    }
    (void)dst;
}

/* ── Public: receive ───────────────────────────────────────────────────────── */

void ipv6_handle_packet(const uint8_t *data, uint16_t len)
{
    if (len < (uint16_t)sizeof(ipv6_header_t)) return;

    const ipv6_header_t *hdr = (const ipv6_header_t *)data;

    /* Check version == 6 */
    if ((hdr->ver_tc_fl >> 28) != 6 &&
        (hdr->ver_tc_fl & 0xF0) != 0x60)  /* big-endian check */
    {
        /* ver_tc_fl is stored in network byte order in the struct,
         * but we read it as a native uint32_t.  On LE: byte0=ver|tc_hi */
        uint8_t ver = data[0] >> 4;
        if (ver != 6) return;
    }

    uint16_t payload_len = ntohs(hdr->payload_len);
    if (len < (uint16_t)sizeof(ipv6_header_t) + payload_len) return;

    const uint8_t *src = hdr->src;
    const uint8_t *dst = hdr->dst;
    const uint8_t *payload = data + sizeof(ipv6_header_t);

    /* Accept packets destined for us: link-local, global, loopback, multicast */
    bool for_us = ipv6_addr_eq(dst, g_linklocal)
               || (g_have_global && ipv6_addr_eq(dst, g_global))
               || (dst[0] == 0xFF)   /* multicast */
               || (dst[15] == 1 && dst[0] == 0 && dst[1] == 0); /* ::1 loopback */

    /* Also accept solicited-node multicast for our addresses */
    if (!for_us) {
        uint8_t snmc[16];
        solicited_node_mc(g_linklocal, snmc);
        if (ipv6_addr_eq(dst, snmc)) for_us = true;
        if (!for_us && g_have_global) {
            solicited_node_mc(g_global, snmc);
            if (ipv6_addr_eq(dst, snmc)) for_us = true;
        }
    }

    if (!for_us) return;

    switch (hdr->next_header) {
    case IPV6_PROTO_TCP:
        tcp6_handle_packet(payload, payload_len, src, dst);
        break;
    case IPV6_PROTO_UDP:
        udp6_handle_packet(payload, payload_len, src, dst);
        break;
    case IPV6_PROTO_ICMPV6:
        /* We don't have the src MAC here; NDP learning happens in eth layer.
         * Pass NULL for src_mac — NA/NS will still work via multicast. */
        icmpv6_handle(payload, payload_len, src, dst, NULL);
        break;
    default:
        break;
    }
}

/* ── Public: send ──────────────────────────────────────────────────────────── */

int ipv6_send_packet(const uint8_t *dst, uint8_t next_header,
                     const void *data, uint16_t len)
{
    netif_t *nif = netif_get();
    if (!nif->up) return -1;

    /* Choose source address */
    const uint8_t *src = g_linklocal;
    if (g_have_global && dst[0] != 0xFE) /* prefer global for non-link-local */
        src = g_global;

    /* Loopback: ::1 */
    static const uint8_t loopback6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (ipv6_addr_eq(dst, loopback6)) {
        /* Build a fake Ethernet frame and enqueue it */
        ipv6_header_t hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.ver_tc_fl   = htonl(0x60000000u);
        hdr.payload_len = htons(len);
        hdr.next_header = next_header;
        hdr.hop_limit   = 64;
        memcpy(hdr.src, src, 16);
        memcpy(hdr.dst, dst, 16);

        uint16_t eth_hdr_len = 14;
        uint8_t frame[eth_hdr_len + sizeof(ipv6_header_t) + len];
        const uint8_t *mac = nic_get_mac();
        if (mac) memcpy(frame, mac, 6); else memset(frame, 0, 6);
        memset(frame + 6, 0, 6);
        frame[12] = 0x86; frame[13] = 0xDD;
        memcpy(frame + eth_hdr_len, &hdr, sizeof(ipv6_header_t));
        if (len && data) memcpy(frame + eth_hdr_len + sizeof(ipv6_header_t), data, len);
        net_rx_enqueue(frame, eth_hdr_len + sizeof(ipv6_header_t) + len);
        return 0;
    }

    /* Determine next-hop MAC */
    uint8_t dst_mac[6];

    /* Multicast: 33:33:... */
    if (dst[0] == 0xFF) {
        mc_mac(dst, dst_mac);
    } else {
        /* Try NDP cache */
        const uint8_t *cached = ndp_lookup(dst);
        if (cached) {
            memcpy(dst_mac, cached, 6);
        } else {
            /* For off-link destinations, use the gateway MAC (ARP for IPv4 gw) */
            netif_t *nif2 = netif_get();
            const arp_entry_t *gw = arp_lookup(nif2->gateway);
            if (!gw) {
                /* Try to resolve gateway */
                arp_send_request(nif2->gateway);
                for (int i = 0; i < 200; i++) {
                    net_poll();
                    gw = arp_lookup(nif2->gateway);
                    if (gw) break;
                    for (volatile int d = 0; d < 50000; d++)
                        __asm__ volatile("pause");
                }
            }
            if (gw) {
                memcpy(dst_mac, gw->mac, 6);
            } else {
                /* Send NS and wait briefly */
                send_ns(dst);
                for (int i = 0; i < 100; i++) {
                    net_poll();
                    cached = ndp_lookup(dst);
                    if (cached) { memcpy(dst_mac, cached, 6); break; }
                    for (volatile int d = 0; d < 50000; d++)
                        __asm__ volatile("pause");
                }
                if (!cached) return -1;
            }
        }
    }

    /* Build IPv6 header */
    ipv6_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.ver_tc_fl   = htonl(0x60000000u);
    hdr.payload_len = htons(len);
    hdr.next_header = next_header;
    hdr.hop_limit   = 64;
    memcpy(hdr.src, src, 16);
    memcpy(hdr.dst, dst, 16);

    uint8_t packet[sizeof(ipv6_header_t) + len];
    memcpy(packet, &hdr, sizeof(ipv6_header_t));
    if (len && data) memcpy(packet + sizeof(ipv6_header_t), data, len);

    klog_puts("[IPV6] TX proto=");
    klog_uint64(next_header);
    klog_puts("\n");

    return eth_send_frame(dst_mac, ETHERTYPE_IPV6, packet,
                          sizeof(ipv6_header_t) + len);
}

/* ── Init ──────────────────────────────────────────────────────────────────── */

void ipv6_init(void)
{
    memset(ndp_cache, 0, sizeof(ndp_cache));
    memset(g_global, 0, sizeof(g_global));
    g_have_global = false;

    netif_t *nif = netif_get();
    make_linklocal(nif->mac, g_linklocal);

    klog_puts("[IPV6] Link-local address assigned\n");
}

const uint8_t *ipv6_get_linklocal(void) { return g_linklocal; }
const uint8_t *ipv6_get_global(void)    { return g_have_global ? g_global : NULL; }

/* ── Ethernet-layer hook to pass src_mac to ICMPv6 ────────────────────────── */
void ipv6_handle_packet_with_mac(const uint8_t *data, uint16_t len,
                                 const uint8_t *src_mac)
{
    if (len < (uint16_t)sizeof(ipv6_header_t)) return;
    const ipv6_header_t *hdr = (const ipv6_header_t *)data;
    uint8_t ver = data[0] >> 4;
    if (ver != 6) return;

    uint16_t payload_len = ntohs(hdr->payload_len);
    if (len < (uint16_t)sizeof(ipv6_header_t) + payload_len) return;

    const uint8_t *src = hdr->src;
    const uint8_t *dst = hdr->dst;
    const uint8_t *payload = data + sizeof(ipv6_header_t);

    /* Learn source MAC */
    if (src_mac && !ipv6_addr_is_zero(src))
        ndp_learn(src, src_mac);

    bool for_us = ipv6_addr_eq(dst, g_linklocal)
               || (g_have_global && ipv6_addr_eq(dst, g_global))
               || (dst[0] == 0xFF);

    if (!for_us) {
        uint8_t snmc[16];
        solicited_node_mc(g_linklocal, snmc);
        if (ipv6_addr_eq(dst, snmc)) for_us = true;
        if (!for_us && g_have_global) {
            solicited_node_mc(g_global, snmc);
            if (ipv6_addr_eq(dst, snmc)) for_us = true;
        }
    }
    if (!for_us) return;

    switch (hdr->next_header) {
    case IPV6_PROTO_TCP:
        tcp6_handle_packet(payload, payload_len, src, dst);
        break;
    case IPV6_PROTO_UDP:
        udp6_handle_packet(payload, payload_len, src, dst);
        break;
    case IPV6_PROTO_ICMPV6:
        icmpv6_handle(payload, payload_len, src, dst, src_mac);
        break;
    default:
        break;
    }
}
