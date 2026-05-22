#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_UDP_SOCKETS 16

typedef struct __attribute__((packed)) {
  uint16_t src_port;
  uint16_t dst_port;
  uint16_t length;
  uint16_t checksum;
} udp_header_t;

// Callback type for incoming UDP packets
typedef void (*udp_recv_cb_t)(uint16_t local_port, const uint8_t *payload,
                              uint16_t length, uint32_t src_ip,
                              uint16_t src_port);

typedef struct {
  uint16_t local_port;
  udp_recv_cb_t callback;
  bool valid;
} udp_socket_t;

// Initialize the UDP layer
void udp_init(void);

// Bind a port to receive incoming UDP packets on that port
int udp_bind(uint16_t port, udp_recv_cb_t callback);

// Unbind a port
void udp_unbind(uint16_t port);

// Send a UDP packet
int udp_send_packet(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                    const void *data, uint16_t len);

// Handle an incoming UDP packet (called from IPv4)
void udp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip,
                       uint32_t dst_ip);

#endif
