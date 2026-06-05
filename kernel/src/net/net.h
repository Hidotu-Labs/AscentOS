#ifndef NET_NET_H
#define NET_NET_H

#include <stdbool.h>
#include <stdint.h>

// Constants
#define ETH_ALEN 6           // Ethernet MAC address length (bytes)
#define ETH_HEADER_LEN 14    // dst(6) + src(6) + ethertype(2)
#define ETH_FRAME_MAX 1518   // Max Ethernet frame (no VLAN tag)
#define ETH_DATA_MAX 1500    // Max Ethernet payload
#define NET_RX_QUEUE_SIZE 256 // RX packet ring entries

// Packet buffer
typedef struct net_packet {
  uint8_t data[ETH_FRAME_MAX];
  uint16_t length;
} net_packet_t;

// Public API

// Initialize the network subsystem: packet queue, netif, ARP.
// Must be called after nic_init().
void net_init(void);

// Called from the NIC IRQ handler to enqueue a received Ethernet frame.
// Copies `len` bytes from `data` into the RX ring. IRQ-safe.
void net_rx_enqueue(const uint8_t *data, uint16_t len);

// Process one queued RX packet (Ethernet → ARP/IPv4 dispatch).
// Returns true if a packet was processed, false if queue is empty.
bool net_poll(void);

#endif
