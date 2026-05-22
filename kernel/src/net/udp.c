#include "net/udp.h"
#include "console/console.h"
#include "console/klog.h"
#include "lib/string.h"
#include "net/byteorder.h"
#include "net/ipv4.h"
#include "net/netif.h"
#include <stdbool.h>
#include <stdint.h>

static udp_socket_t sockets[MAX_UDP_SOCKETS];

void udp_init(void) { memset(sockets, 0, sizeof(sockets)); }

int udp_bind(uint16_t port, udp_recv_cb_t callback) {
  if (port == 0) {
    // Assign ephemeral port
    static uint16_t next_ephemeral = 32768;
    for (int i = 0; i < 1024; i++) {
      uint16_t p = next_ephemeral++;
      if (next_ephemeral == 0)
        next_ephemeral = 32768;

      bool used = false;
      for (int j = 0; j < MAX_UDP_SOCKETS; j++) {
        if (sockets[j].valid && sockets[j].local_port == p) {
          used = true;
          break;
        }
      }
      if (!used) {
        port = p;
        break;
      }
    }
    if (port == 0)
      return -1;
  }

  // Check if already bound
  for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
    if (sockets[i].valid && sockets[i].local_port == port) {
      return -1; // Already bound
    }
  }

  // Find empty slot
  for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
    if (!sockets[i].valid) {
      sockets[i].local_port = port;
      sockets[i].callback = callback;
      sockets[i].valid = true;
      return port; // Return the actual port bound
    }
  }
  return -1; // No slots left
}

void udp_unbind(uint16_t port) {
  for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
    if (sockets[i].valid && sockets[i].local_port == port) {
      sockets[i].valid = false;
      sockets[i].callback = NULL;
      return;
    }
  }
}

void udp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip,
                       uint32_t dst_ip) {
  (void)dst_ip;
  if (len < sizeof(udp_header_t))
    return;

  const udp_header_t *hdr = (const udp_header_t *)data;
  uint16_t src_port = ntohs(hdr->src_port);
  uint16_t dst_port = ntohs(hdr->dst_port);
  uint16_t udp_len = ntohs(hdr->length);

  if (len < udp_len)
    return; // Truncated packet

  klog_puts("[UDP] RX from ");
  klog_uint64(src_ip);
  klog_puts(":");
  klog_uint64(src_port);
  klog_puts(" to ");
  klog_uint64(dst_ip);
  klog_puts(":");
  klog_uint64(dst_port);
  klog_puts("\n");

  const uint8_t *payload = data + sizeof(udp_header_t);
  uint16_t payload_len = udp_len - sizeof(udp_header_t);

  // Find a listening socket
  for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
    if (sockets[i].valid && sockets[i].local_port == dst_port) {
      if (sockets[i].callback) {
        sockets[i].callback(sockets[i].local_port, payload, payload_len, src_ip,
                            src_port);
      }
      return;
    }
  }

  // No listener bound, just drop silently
}

int udp_send_packet(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                    const void *data, uint16_t len) {
  uint8_t packet[sizeof(udp_header_t) + len];
  udp_header_t *hdr = (udp_header_t *)packet;

  hdr->src_port = htons(src_port);
  hdr->dst_port = htons(dst_port);
  hdr->length = htons(sizeof(udp_header_t) + len);
  hdr->checksum = 0; // UDP checksum is optional (0 means unused)

  memcpy(packet + sizeof(udp_header_t), data, len);

  return ipv4_send_packet(dst_ip, PROTO_UDP, packet,
                          sizeof(udp_header_t) + len);
}
