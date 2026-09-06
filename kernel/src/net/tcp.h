#ifndef NET_TCP_H
#define NET_TCP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define TCP_MAX_TCBS 64
#define TCP_RX_BUFFER_SIZE 524288
#define TCP_DEFAULT_WINDOW 65535
enum tcp_state { TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED, TCP_ESTABLISHED, TCP_FIN_WAIT_1,
  TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_TIME_WAIT, TCP_RESET };

struct tcp_tcb {
  bool used; enum tcp_state state;
  uint8_t address_family;
  uint32_t local_ip, remote_ip; uint16_t local_port, remote_port;
  uint8_t local_ip6[16], remote_ip6[16];
  uint32_t snd_una, snd_nxt, rcv_nxt; uint16_t snd_wnd, rcv_wnd, mss;
  uint8_t retries; uint64_t deadline;
  uint8_t rx_buffer[TCP_RX_BUFFER_SIZE];
  uint8_t tx_buffer[1460]; size_t tx_length; uint32_t tx_seq; uint8_t tx_flags;
  size_t rx_head, rx_tail; bool peer_closed; int error;
  uint8_t unacked_packets;
  void *wait_queue; void *vfs_node;
  struct tcp_tcb *listener;
  struct tcp_tcb *accept_queue[8]; size_t accept_head, accept_tail; int backlog;
};

struct tcp_stats {
  uint64_t rx_segments, bad_checksum, malformed, resets, retransmits;
  uint64_t duplicates, out_of_order, timeouts;
};

/* Snapshot of one TCP connection for /proc/net/tcp */
struct tcp_entry_snapshot {
  uint32_t local_ip, remote_ip;
  uint16_t local_port, remote_port;
  uint8_t  state;   /* enum tcp_state value */
};

/* Fill up to 'max' entries; returns number filled. */
int tcp_get_snapshot(struct tcp_entry_snapshot *out, int max);

void tcp_init(void);
void tcp_input_ipv4(uint32_t src, uint32_t dst, const uint8_t *, size_t);
void tcp_input_ipv6(const uint8_t src[16], const uint8_t dst[16],
                    const uint8_t *, size_t);
void tcp_timer_tick(uint64_t now);
const struct tcp_stats *tcp_get_stats(void);
struct tcp_tcb *tcp_alloc(void);
void tcp_free(struct tcp_tcb *);
int tcp_active_open(struct tcp_tcb *, uint32_t, uint16_t);
int tcp_active_open6(struct tcp_tcb *, const uint8_t[16], uint16_t);
int tcp_bind(struct tcp_tcb *, uint32_t, uint16_t);
int tcp_bind6(struct tcp_tcb *, const uint8_t[16], uint16_t);
int tcp_send(struct tcp_tcb *, const void *, size_t);
int tcp_recv(struct tcp_tcb *, void *, size_t, bool);
int tcp_close(struct tcp_tcb *);
int tcp_listen(struct tcp_tcb *, int);
struct tcp_tcb *tcp_accept(struct tcp_tcb *, bool);
bool tcp_readable(const struct tcp_tcb *);
bool tcp_writable(const struct tcp_tcb *);
bool net_phase8_init(void);
#endif
