#include "net/dhcp.h"
#include "console/console.h"
#include "drivers/timer/pit.h"
#include "lib/string.h"
#include "net/byteorder.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "net/netif.h"
#include "net/udp.h"
#include "sched/sched.h"

static uint32_t current_xid = 0xAA55CC33;
static volatile bool dhcp_done = false;
static uint32_t offered_ip = 0;
static uint32_t server_ip = 0;
static uint32_t offered_router = 0;
static uint32_t offered_netmask = 0;
static int dhcp_state =
    0; // 0=Init, 1=DiscoverSent, 2=OfferReceived, 3=RequestSent, 4=AckReceived

static void dhcp_send_discover(void) {
  dhcp_packet_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.op = DHCP_OP_REQUEST;
  pkt.htype = 1; // Ethernet
  pkt.hlen = 6;  // MAC length
  pkt.hops = 0;
  pkt.xid = htonl(current_xid);
  pkt.secs = 0;
  pkt.flags = htons(0x8000); // Broadcast flag

  netif_t *nif = netif_get();
  memcpy(pkt.chaddr, nif->mac, 6);
  pkt.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

  uint8_t *opt = pkt.options;
  // DHCP Message Type = DISCOVER
  *opt++ = 53;
  *opt++ = 1;
  *opt++ = DHCP_MSG_DISCOVER;
  // End option
  *opt++ = 255;

  udp_send_packet(IP4(255, 255, 255, 255), DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                  &pkt, sizeof(pkt));
  dhcp_state = 1;
}

static void dhcp_send_request(void) {
  dhcp_packet_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.op = DHCP_OP_REQUEST;
  pkt.htype = 1;
  pkt.hlen = 6;
  pkt.xid = htonl(current_xid);
  pkt.flags = htons(0x8000); // Broadcast

  netif_t *nif = netif_get();
  memcpy(pkt.chaddr, nif->mac, 6);
  pkt.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

  uint8_t *opt = pkt.options;
  // DHCP Message Type = REQUEST
  *opt++ = 53;
  *opt++ = 1;
  *opt++ = DHCP_MSG_REQUEST;

  // Requested IP Address
  *opt++ = 50;
  *opt++ = 4;
  memcpy(opt, &offered_ip, 4);
  opt += 4;

  // Server Identifier
  *opt++ = 54;
  *opt++ = 4;
  memcpy(opt, &server_ip, 4);
  opt += 4;

  // Parameter Request List
  *opt++ = 55;
  *opt++ = 2;
  *opt++ = 1;
  *opt++ = 3; // Subnet Mask, Router

  // End option
  *opt++ = 255;

  udp_send_packet(IP4(255, 255, 255, 255), DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                  &pkt, sizeof(pkt));
  dhcp_state = 3;
}

static void dhcp_recv_cb(uint16_t local_port, const uint8_t *payload,
                         uint16_t length, uint32_t src_ip, uint16_t src_port) {
  (void)local_port;
  (void)src_ip;
  (void)src_port;
  if (length < sizeof(dhcp_packet_t) - 308)
    return;

  dhcp_packet_t *pkt = (dhcp_packet_t *)payload;
  if (pkt->op != DHCP_OP_REPLY)
    return;
  if (ntohl(pkt->xid) != current_xid) {
    // console_puts("[DHCP] XID mismatch: expected 0x");
    // klog_hex32(current_xid);
    // console_puts(" got 0x");
    // klog_hex32(ntohl(pkt->xid));
    // console_puts("\n");
    return;
  }

  if (ntohl(pkt->magic_cookie) != 0x63825363) {
    console_puts("[DHCP] Invalid magic cookie!\n");
    return;
  }

  uint8_t msg_type = 0;
  uint32_t router = 0;
  uint32_t netmask = 0;
  uint32_t server_id = 0;

  int i = 0;
  while (i < 308 && pkt->options[i] != 255) {
    if (pkt->options[i] == 0) { // Pad option
      i++;
      continue;
    }
    uint8_t opt = pkt->options[i];
    uint8_t opt_len = pkt->options[i + 1];

    switch (opt) {
    case 53:
      msg_type = pkt->options[i + 2];
      break;
    case 54:
      memcpy(&server_id, &pkt->options[i + 2], 4);
      break;
    case 1:
      memcpy(&netmask, &pkt->options[i + 2], 4);
      break;
    case 3:
      memcpy(&router, &pkt->options[i + 2], 4);
      break; // Assuming first router
    }
    i += 2 + opt_len;
  }

  if (msg_type == DHCP_MSG_OFFER && dhcp_state == 1) {
    offered_ip = pkt->yiaddr;
    server_ip = server_id;
    dhcp_state = 2;
    console_puts("[DHCP] State: OFFER received.\n");
  } else if (msg_type == DHCP_MSG_ACK && (dhcp_state == 3 || dhcp_state == 2)) {
    offered_netmask = netmask;
    offered_router = router;
    dhcp_state = 4;
    dhcp_done = true;
    console_puts("[DHCP] State: ACK received.\n");
  } else if (msg_type == DHCP_MSG_NAK) {
    console_puts("[DHCP] Received NAK from server.\n");
  } else {
    // Hidden debug info to avoid terminal clutter unless manually enabled
    /*
    console_puts("[DHCP] RX msg=");
    klog_uint64(msg_type);
    console_puts(" state=");
    klog_uint64(dhcp_state);
    console_puts(" xid=0x");
    klog_hex32(ntohl(pkt->xid));
    console_puts("\n");
    */
  }
}

void dhcp_init(void) { udp_bind(DHCP_CLIENT_PORT, dhcp_recv_cb); }

bool dhcp_negotiate(void) {
  dhcp_done = false;
  dhcp_state = 0;
  current_xid++;

  // Retry Discover up to 3 times
  for (int attempts = 0; attempts < 3 && dhcp_state < 2; attempts++) {
    console_puts("[DHCP] Sending DISCOVER...\n");
    dhcp_send_discover();
    uint64_t start = pit_get_ticks();
    while (dhcp_state < 2 && (pit_get_ticks() - start < 200)) { // 2s timeout
      net_poll();
      sched_yield();
    }
  }

  if (dhcp_state == 2) {
    console_puts("[DHCP] Received OFFER...\n");
    // Retry Request up to 3 times
    for (int attempts = 0; attempts < 3 && dhcp_state < 4; attempts++) {
      console_puts("[DHCP] Sending REQUEST...\n");
      dhcp_send_request();
      uint64_t start = pit_get_ticks();
      while (dhcp_state < 4 && (pit_get_ticks() - start < 200)) { // 2s timeout
        net_poll();
        sched_yield();
      }
    }
  }

  if (dhcp_done) {
    netif_t *nif = netif_get();
    nif->ip = ntohl(offered_ip);
    nif->netmask = ntohl(offered_netmask);
    nif->gateway = ntohl(offered_router);
    nif->up = true;

    console_puts("[DHCP] Received ACK: Successfully bound!\n");
    return true;
  }

  console_puts("[DHCP] Negotiation failed.\n");
  return false;
}
