/*
 * ARP — Address Resolution Protocol
 *
 * Maintains a simple 16-entry cache mapping IPv4 addresses to MAC addresses.
 * Handles incoming ARP requests (replies to queries for our IP) and ARP
 * replies (populates the cache).
 */

#include "net/arp.h"
#include "net/ethernet.h"
#include "net/netif.h"
#include "net/byteorder.h"
#include "lib/string.h"
#include "console/console.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ARP cache
static arp_entry_t arp_table[ARP_TABLE_SIZE];

void arp_init(void) {
    memset(arp_table, 0, sizeof(arp_table));
}

// Cache operations

// Update or insert an entry in the ARP cache
static void arp_cache_update(uint32_t ip, const uint8_t *mac) {
    const char *hex = "0123456789ABCDEF";

    // First, check if we already have this IP
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            if (memcmp(arp_table[i].mac, mac, 6) != 0) {
                memcpy(arp_table[i].mac, mac, 6);
                console_puts("[ARP] Updated entry for ");
                // ... (Helper would be nice, but we'll just log)
            }
            return;
        }
    }

    // Find an empty slot
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip = ip;
            memcpy(arp_table[i].mac, mac, 6);
            arp_table[i].valid = true;
            
            console_puts("[ARP] Learned ");
            for (int j = 0; j < 6; j++) {
                console_putchar(hex[(mac[j] >> 4) & 0xF]);
                console_putchar(hex[mac[j] & 0xF]);
                if (j < 5) console_putchar(':');
            }
            console_puts("\n");
            return;
        }
    }

    // Table full — overwrite slot 0 (simplest eviction policy)
    arp_table[0].ip = ip;
    memcpy(arp_table[0].mac, mac, 6);
    arp_table[0].valid = true;
}

const arp_entry_t *arp_lookup(uint32_t ip) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            return &arp_table[i];
        }
    }
    return NULL;
}

const arp_entry_t *arp_get_table(int *count) {
    if (count) *count = ARP_TABLE_SIZE;
    return arp_table;
}

// Send an ARP reply

static void arp_send_reply(const uint8_t *target_mac, uint32_t target_ip_net) {
    netif_t *nif = netif_get();

    arp_packet_t reply;
    reply.hw_type    = htons(ARP_HW_ETHERNET);
    reply.proto_type = htons(ARP_PROTO_IPV4);
    reply.hw_len     = 6;
    reply.proto_len  = 4;
    reply.operation  = htons(ARP_OP_REPLY);

    memcpy(reply.sender_mac, nif->mac, 6);
    reply.sender_ip = htonl(nif->ip);

    memcpy(reply.target_mac, target_mac, 6);
    reply.target_ip = target_ip_net;  // Already in network byte order

    eth_send_frame(target_mac, ETHERTYPE_ARP, &reply, sizeof(reply));
}

// Send an ARP request

int arp_send_request(uint32_t target_ip) {
    netif_t *nif = netif_get();
    if (!nif->up) return -1;

    arp_packet_t req;
    req.hw_type    = htons(ARP_HW_ETHERNET);
    req.proto_type = htons(ARP_PROTO_IPV4);
    req.hw_len     = 6;
    req.proto_len  = 4;
    req.operation  = htons(ARP_OP_REQUEST);

    memcpy(req.sender_mac, nif->mac, 6);
    req.sender_ip = htonl(nif->ip);

    // Target MAC is zero (unknown — that's what we're asking for)
    memset(req.target_mac, 0, 6);
    req.target_ip = htonl(target_ip);

    // Send to broadcast
    return eth_send_frame(ETH_BROADCAST, ETHERTYPE_ARP, &req, sizeof(req));
}

// Handle an incoming ARP packet

void arp_handle_packet(const uint8_t *data, uint16_t len) {
    if (len < sizeof(arp_packet_t)) return;

    const arp_packet_t *pkt = (const arp_packet_t *)data;

    // Validate: must be Ethernet/IPv4
    if (ntohs(pkt->hw_type) != ARP_HW_ETHERNET) return;
    if (ntohs(pkt->proto_type) != ARP_PROTO_IPV4) return;
    if (pkt->hw_len != 6 || pkt->proto_len != 4) return;

    netif_t *nif = netif_get();
    uint32_t sender_ip = ntohl(pkt->sender_ip);
    uint32_t target_ip = ntohl(pkt->target_ip);

    uint16_t op = ntohs(pkt->operation);

    // Always learn the sender's IP→MAC mapping (RFC 826)
    if (sender_ip != 0) {
        arp_cache_update(sender_ip, pkt->sender_mac);
    }

    if (op == ARP_OP_REQUEST) {
        // Is someone asking for OUR IP?
        if (target_ip == nif->ip) {
            arp_send_reply(pkt->sender_mac, pkt->sender_ip);
        }
    }
    // ARP_OP_REPLY: the cache was already updated above
}
