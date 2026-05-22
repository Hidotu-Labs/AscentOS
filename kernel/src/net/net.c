/*
 * Network subsystem core — packet queue, netif, and main dispatch loop.
 */

#include "net/net.h"
#include "console/console.h"
#include "drivers/net/nic.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "net/arp.h"
#include "net/dhcp.h"
#include "net/ethernet.h"
#include "net/netif.h"
#include "net/tcp.h"
#include "net/udp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── RX packet ring buffer ───────────────────────────────────────────────────
// Single-producer (IRQ handler) / single-consumer (net_poll) ring buffer.
// No lock needed because head is only written by producer and tail by consumer.

static net_packet_t rx_ring[NET_RX_QUEUE_SIZE];
static volatile uint32_t rx_head = 0; // Written by IRQ (producer)
static volatile uint32_t rx_tail = 0; // Written by net_poll (consumer)
static spinlock_t net_rx_lock = SPINLOCK_INIT;

// ── Global network interface ────────────────────────────────────────────────
static netif_t g_netif = {0};

netif_t *netif_get(void) { return &g_netif; }

void netif_configure(uint32_t ip, uint32_t gateway, uint32_t netmask) {
  g_netif.ip = ip;
  g_netif.gateway = gateway;
  g_netif.netmask = netmask;
  g_netif.up = true;
}

// ── Packet queue ────────────────────────────────────────────────────────────

void net_rx_enqueue(const uint8_t *data, uint16_t len) {
  if (len == 0 || len > ETH_FRAME_MAX)
    return;

  spinlock_acquire(&net_rx_lock);
  uint32_t next_head = (rx_head + 1) % NET_RX_QUEUE_SIZE;

  // Drop packet if queue is full
  if (next_head == rx_tail) {
    spinlock_release(&net_rx_lock);
    return;
  }

  memcpy(rx_ring[rx_head].data, data, len);
  rx_ring[rx_head].length = len;

  rx_head = next_head;
  spinlock_release(&net_rx_lock);
}

bool net_poll(void) {
  nic_poll();

  spinlock_acquire(&net_rx_lock);
  if (rx_tail == rx_head) {
    spinlock_release(&net_rx_lock);
    return false; // Queue empty
  }

  net_packet_t *pkt = &rx_ring[rx_tail];
  uint8_t data[ETH_FRAME_MAX];
  uint16_t len = pkt->length;
  memcpy(data, pkt->data, len);

  rx_tail = (rx_tail + 1) % NET_RX_QUEUE_SIZE;
  spinlock_release(&net_rx_lock);

  // Dispatch through the Ethernet handler (outside the lock!)
  eth_handle_frame(data, len);

  return true;
}

// ── Initialization ──────────────────────────────────────────────────────────

void net_init(void) {
  // Clear the packet queue
  rx_head = 0;
  rx_tail = 0;
  spinlock_init(&net_rx_lock);
  memset(rx_ring, 0, sizeof(rx_ring));

  // Copy MAC from the NIC driver into the netif
  const uint8_t *mac = nic_get_mac();
  if (mac) {
    memcpy(g_netif.mac, mac, 6);
  }

  // Start with empty IP configuration
  netif_configure(0, 0, 0);

  // Initialize ARP subsystem
  arp_init();

  // Initialize UDP subsystem
  udp_init();

  // Initialize TCP subsystem
  tcp_init();

  // Initialize DHCP and negotiate
  dhcp_init();
  if (!dhcp_negotiate()) {
    console_puts("[DHCP] Falling back to static IP 10.0.2.15.\n");
    netif_configure(IP4(10, 0, 2, 15), IP4(10, 0, 2, 2), IP4(255, 255, 255, 0));
  }

  console_puts("[OK] Network stack initialized.\n");
}
