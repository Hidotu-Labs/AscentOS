#include "net/ipv4.h"
#include "console/console.h"
#include "console/klog.h"
#include "lib/string.h"
#include "net/arp.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/net.h"
#include "net/netif.h"
#include "net/tcp.h"
#include "drivers/net/nic.h"
#include "net/udp.h"

static uint16_t next_id = 1;


void ipv4_handle_packet(const uint8_t *data, uint16_t len) {
  if (len < sizeof(ipv4_header_t))
    return;

  ipv4_header_t *hdr = (ipv4_header_t *)data;

  if ((hdr->version_ihl >> 4) != 4)
    return;

  uint8_t ihl = (hdr->version_ihl & 0x0F) * 4;
  if (len < ihl)
    return;

  // Verify Checksum
  uint16_t received_checksum = hdr->checksum;
  ((ipv4_header_t *)hdr)->checksum = 0;
  uint16_t computed = calculate_checksum(hdr, ihl);
  if (computed != received_checksum) {
    hdr->checksum = received_checksum;
    return;
  }
  hdr->checksum = received_checksum;

  netif_t *nif = netif_get();
  uint32_t dst_ip = ntohl(hdr->dst_ip);

  // Filter packets not for us (and not broadcast)
  if (dst_ip != nif->ip && dst_ip != 0xFFFFFFFF && dst_ip != 0x7F000001) {
    return;
  }

  const uint8_t *payload = data + ihl;
  uint16_t payload_len = ntohs(hdr->length) - ihl;

  switch (hdr->protocol) {
  case PROTO_ICMP:
    icmp_handle_packet(payload, payload_len, ntohl(hdr->src_ip));
    break;
  case PROTO_UDP:
    udp_handle_packet(payload, payload_len, ntohl(hdr->src_ip), dst_ip);
    break;
  case PROTO_TCP:
    klog_puts("[IPV4] TCP RX from ");
    klog_uint64(ntohl(hdr->src_ip));
    klog_puts(" len=");
    klog_uint64(payload_len);
    klog_puts("\n");
    tcp_handle_packet(payload, payload_len, ntohl(hdr->src_ip), dst_ip);
    break;
  default:
    break;
  }
}

int ipv4_send_packet(uint32_t dst_ip, uint8_t protocol, const void *data,
                     uint16_t len) {
  netif_t *nif = netif_get();
  if (!nif->up)
    return -1;

  if (dst_ip == 0x7F000001 || dst_ip == nif->ip) {
    ipv4_header_t hdr;
    hdr.version_ihl = (4 << 4) | 5;
    hdr.tos = 0;
    hdr.length = htons(sizeof(ipv4_header_t) + len);
    hdr.id = htons(next_id++);
    hdr.flags_offset = 0;
    hdr.ttl = 64;
    hdr.protocol = protocol;
    hdr.src_ip = htonl(dst_ip); // Loopback: source = destination
    hdr.dst_ip = htonl(dst_ip);
    hdr.checksum = 0;
    hdr.checksum = calculate_checksum(&hdr, sizeof(ipv4_header_t));

    uint8_t packet[sizeof(ipv4_header_t) + len];
    memcpy(packet, &hdr, sizeof(ipv4_header_t));
    if (len > 0 && data) {
      memcpy(packet + sizeof(ipv4_header_t), data, len);
    }

    // Construct a dummy Ethernet frame for the RX queue
    uint16_t eth_hdr_len = 14;
    uint8_t frame[eth_hdr_len + sizeof(ipv4_header_t) + len];
    
    // Use the NIC's MAC for the destination so ethernet.c accepts it
    const uint8_t *mac = nic_get_mac();
    if (mac) {
      memcpy(frame, mac, 6);
    } else {
      memset(frame, 0, 6);
    }
    
    // Source MAC can be any (or the same)
    memset(frame + 6, 0, 6);
    
    // EtherType: IPv4 (big endian)
    frame[12] = 0x08;
    frame[13] = 0x00;

    // IP payload
    memcpy(frame + eth_hdr_len, &hdr, sizeof(ipv4_header_t));
    if (len > 0 && data) {
      memcpy(frame + eth_hdr_len + sizeof(ipv4_header_t), data, len);
    }

    // BREAK RECURSION: Enqueue as a standard Ethernet frame
    net_rx_enqueue(frame, eth_hdr_len + sizeof(ipv4_header_t) + len);
    return 0;
  }

  // Routing: if destination is off-subnet, send via gateway
  uint32_t next_hop = dst_ip;
  if (dst_ip != 0xFFFFFFFF &&
      (dst_ip & nif->netmask) != (nif->ip & nif->netmask)) {
    next_hop = nif->gateway;
  }

  uint8_t dst_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  if (dst_ip != 0xFFFFFFFF) {
    // Determine next-hop MAC via ARP
    const arp_entry_t *entry = arp_lookup(next_hop);
    if (!entry) {
      arp_send_request(next_hop);
      // Wait up to 100ms for reply
      for (int i = 0; i < 100; i++) {
        net_poll();
        entry = arp_lookup(next_hop);
        if (entry)
          break;
        // Small delay (~1ms)
        for (volatile int d = 0; d < 50000; d++)
          __asm__ volatile("pause");
      }
      if (!entry)
        return -1;
    }
    memcpy(dst_mac, entry->mac, 6);
  }

  ipv4_header_t hdr;
  hdr.version_ihl = (4 << 4) | 5; // Ver 4, IHL 5 (20 bytes)
  hdr.tos = 0;
  hdr.length = htons(sizeof(ipv4_header_t) + len);
  hdr.id = htons(next_id++);
  hdr.flags_offset = 0;
  hdr.ttl = 64;
  hdr.protocol = protocol;
  hdr.src_ip = htonl(nif->ip);
  hdr.dst_ip = htonl(dst_ip);
  hdr.checksum = 0;
  hdr.checksum = calculate_checksum(&hdr, sizeof(ipv4_header_t));

  klog_puts("[IPV4] Sending proto ");
  klog_uint64(protocol);
  klog_puts(" to ");
  klog_uint64(dst_ip);
  klog_puts("\n");

  // Allocate buffer for frame
  uint8_t packet[sizeof(ipv4_header_t) + len];
  memcpy(packet, &hdr, sizeof(ipv4_header_t));
  if (len > 0 && data) {
    memcpy(packet + sizeof(ipv4_header_t), data, len);
  }

  int ret = eth_send_frame(dst_mac, ETHERTYPE_IPV4, packet,
                           sizeof(ipv4_header_t) + len);
  return ret;
}
