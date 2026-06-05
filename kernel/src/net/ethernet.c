/*
 * Ethernet framing — build and parse 14-byte Ethernet II headers.
 */

#include "net/ethernet.h"
#include "net/net.h"
#include "net/netif.h"
#include "net/arp.h"
#include "net/ipv4.h"
#include "net/ipv6.h"
#include "net/byteorder.h"
#include "drivers/net/nic.h"
#include "console/console.h"
#include "console/klog.h"
#include "lib/string.h"

#include <stddef.h>
#include <stdint.h>

// Broadcast MAC
const uint8_t ETH_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Send

int eth_send_frame(const uint8_t *dst_mac, uint16_t ethertype,
                   const void *payload, uint16_t payload_len) {
    if (payload_len > ETH_DATA_MAX) return -1;

    // Build the frame in a stack buffer
    uint8_t frame[ETH_FRAME_MAX];
    eth_header_t *hdr = (eth_header_t *)frame;

    // Destination MAC
    memcpy(hdr->dst, dst_mac, 6);

    // Source MAC (our NIC)
    netif_t *nif = netif_get();
    memcpy(hdr->src, nif->mac, 6);

    // EtherType in network byte order
    hdr->ethertype = htons(ethertype);

    // Copy payload after the header
    memcpy(frame + ETH_HEADER_LEN, payload, payload_len);

    uint16_t total_len = ETH_HEADER_LEN + payload_len;

    // Ethernet minimum frame is 60 bytes (without CRC); pad if needed.
    // The NIC hardware appends the 4-byte CRC automatically.
    if (total_len < 60) {
        memset(frame + total_len, 0, 60 - total_len);
        total_len = 60;
    }

    int ret = nic_send(frame, total_len);
    return ret;
}

// Receive dispatch

void eth_handle_frame(const uint8_t *data, uint16_t len) {
    if (len < ETH_HEADER_LEN) return;  // Runt frame

    const eth_header_t *hdr = (const eth_header_t *)data;
    uint16_t ethertype = ntohs(hdr->ethertype);

    const uint8_t *payload = data + ETH_HEADER_LEN;
    uint16_t payload_len = len - ETH_HEADER_LEN;

    switch (ethertype) {
    case ETHERTYPE_ARP:
        arp_handle_packet(payload, payload_len);
        break;

    case ETHERTYPE_IPV4:
        ipv4_handle_packet(payload, payload_len);
        break;

    case ETHERTYPE_IPV6:
        ipv6_handle_packet_with_mac(payload, payload_len, hdr->src);
        break;

    default:
        // Unknown EtherType — silently drop
        break;
    }
}
