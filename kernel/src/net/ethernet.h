#ifndef NET_ETHERNET_H
#define NET_ETHERNET_H

#include <stdint.h>

// ── EtherType values (in host byte order — convert with htons before wire) ──
#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV6  0x86DD

// ── Ethernet frame header (14 bytes) ────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  dst[6];       // Destination MAC address
    uint8_t  src[6];       // Source MAC address
    uint16_t ethertype;    // EtherType (network byte order on wire)
} eth_header_t;

// Broadcast MAC address
extern const uint8_t ETH_BROADCAST[6];

// Send an Ethernet frame: builds header with our MAC as source, appends payload.
// `ethertype` is in HOST byte order (e.g. ETHERTYPE_ARP = 0x0806).
// Returns 0 on success, -1 on error.
int eth_send_frame(const uint8_t *dst_mac, uint16_t ethertype,
                   const void *payload, uint16_t payload_len);

// Handle a received Ethernet frame — dispatches to ARP or IPv4 handler.
void eth_handle_frame(const uint8_t *data, uint16_t len);

#endif
