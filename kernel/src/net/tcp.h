#ifndef NET_TCP_H
#define NET_TCP_H

#include "../sched/wait.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

#define PROTO_TCP 6

typedef struct {
  uint16_t src_port;
  uint16_t dst_port;
  uint32_t seq_num;
  uint32_t ack_num;
  uint8_t
      data_offset; // Top 4 bits are length (in 32-bit words), bottom 4 reserved
  uint8_t flags;
  uint16_t window;
  uint16_t checksum;
  uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

typedef enum {
  TCP_STATE_CLOSED,
  TCP_STATE_LISTEN,
  TCP_STATE_SYN_SENT,
  TCP_STATE_SYN_RCVD,
  TCP_STATE_ESTABLISHED,
  TCP_STATE_FIN_WAIT1,
  TCP_STATE_FIN_WAIT2
} tcp_state_t;

#define MAX_TCP_SOCKETS 32
#define TCP_MAX_BACKLOG 8

typedef void (*tcp_recv_cb_t)(int sock_id, const uint8_t *payload,
                              uint16_t length);

typedef struct {
  volatile bool valid;
  volatile tcp_state_t state;

  uint32_t local_ip;
  uint16_t local_port;
  uint32_t remote_ip;
  uint16_t remote_port;

  uint32_t seq_num; // Our next sequence number
  uint32_t ack_num; // Their next sequence number we expect to ACK

  tcp_recv_cb_t recv_callback;
  void (*event_callback)(int sock_id);

  // Server mode fields
  int parent_sock_id; // If spawned from a listening socket
  int accept_queue[TCP_MAX_BACKLOG];
  int accept_count;

  wait_queue_t wait_queue;
} tcp_socket_t;

void tcp_init(void);
int tcp_listen(uint16_t port, tcp_recv_cb_t on_recv);
int tcp_connect(uint32_t ip, uint16_t port, tcp_recv_cb_t on_recv);
int tcp_send(int sock_id, const void *data, uint16_t len);
void tcp_close(int sock_id);
void tcp_handle_packet(const uint8_t *payload, uint16_t length, uint32_t src_ip,
                       uint32_t dst_ip);

int tcp_accept(int sock_id);
int tcp_get_remote_info(int sock_id, uint32_t *ip, uint16_t *port);
void tcp_set_callbacks(int sock_id, tcp_recv_cb_t rcb,
                       void (*ecb)(int sock_id));

#endif
