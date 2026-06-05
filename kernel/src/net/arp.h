#ifndef NET_ARP_H
#define NET_ARP_H

#include <stdbool.h>
#include <stdint.h>

// Constants
#define ARP_TABLE_SIZE  16
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

#define ARP_HW_ETHERNET 1
#define ARP_PROTO_IPV4   0x0800

// ARP packet (28 bytes for Ethernet + IPv4)
typedef struct __attribute__((packed)) {
    uint16_t hw_type;        // Hardware type (1 = Ethernet)
    uint16_t proto_type;     // Protocol type (0x0800 = IPv4)
    uint8_t  hw_len;         // Hardware address length (6)
    uint8_t  proto_len;      // Protocol address length (4)
    uint16_t operation;      // 1 = request, 2 = reply
    uint8_t  sender_mac[6];  // Sender hardware address
    uint32_t sender_ip;      // Sender protocol address (network byte order)
    uint8_t  target_mac[6];  // Target hardware address
    uint32_t target_ip;      // Target protocol address (network byte order)
} arp_packet_t;

// ARP cache entry
typedef struct {
    uint32_t ip;             // Host byte order
    uint8_t  mac[6];
    bool     valid;
} arp_entry_t;

// Public API

// Initialize the ARP subsystem (clears the cache)
void arp_init(void);

// Handle an incoming ARP packet (called from Ethernet dispatch).
// `data` points to the ARP payload (after the Ethernet header).
void arp_handle_packet(const uint8_t *data, uint16_t len);

// Send an ARP "who-has" request for the given IP (host byte order).
// Returns 0 on success, -1 on error.
int arp_send_request(uint32_t target_ip);

// Look up a MAC address for the given IP (host byte order).
// Returns a pointer to the ARP entry if found, or NULL.
const arp_entry_t *arp_lookup(uint32_t ip);

// Get the entire ARP table for display purposes.
// Sets *count to ARP_TABLE_SIZE.
const arp_entry_t *arp_get_table(int *count);

#endif
