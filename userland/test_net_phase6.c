/* Phase 6 IPv4/UDP userland integration test. */
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_HOST "10.0.2.2"
#define DEFAULT_PORT 9000
#define WAIT_MS 3000

static int passed, failed;
#define CHECK(x, name) do { if (x) { printf("[PASS] %s\n", name); passed++; } \
  else { printf("[FAIL] %s (errno=%d: %s)\n", name, errno, strerror(errno)); failed++; } } while (0)

static struct sockaddr_in addr4(const char *ip, unsigned short port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
    fprintf(stderr, "invalid IPv4 address: %s\n", ip);
    exit(2);
  }
  return a;
}

static int readable(int fd) {
  struct pollfd p = { .fd = fd, .events = POLLIN };
  return poll(&p, 1, WAIT_MS) == 1 && (p.revents & POLLIN);
}

static int echo_once(int fd, const struct sockaddr_in *peer,
                     const char *text, int connected, int message_api) {
  char buf[128];
  ssize_t sent, got;
  if (message_api) {
    struct iovec iov = { .iov_base = (void *)text, .iov_len = strlen(text) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = connected ? NULL : (void *)peer;
    msg.msg_namelen = connected ? 0 : sizeof(*peer);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    sent = sendmsg(fd, &msg, 0);
  } else if (connected) {
    sent = send(fd, text, strlen(text), 0);
  } else {
    sent = sendto(fd, text, strlen(text), 0,
                  (const struct sockaddr *)peer, sizeof(*peer));
  }
  if (sent != (ssize_t)strlen(text) || !readable(fd)) return -1;
  memset(buf, 0, sizeof(buf));
  if (message_api) {
    struct sockaddr_in src;
    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &src;
    msg.msg_namelen = sizeof(src);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    got = recvmsg(fd, &msg, 0);
  } else {
    got = recv(fd, buf, sizeof(buf), 0);
  }
  return got == sent && !memcmp(buf, text, (size_t)got) ? 0 : -1;
}

static void local_abi(const struct sockaddr_in *peer) {
  struct sockaddr_in any = addr4("0.0.0.0", 0);
  struct sockaddr_in named = addr4("0.0.0.0", 19006), got;
  socklen_t len;
  int a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int b = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  CHECK(a >= 0 && b >= 0, "AF_INET UDP socket creation");
  if (a < 0 || b < 0) goto out;
  CHECK(bind(a, (struct sockaddr *)&any, sizeof(any)) == 0,
        "wildcard bind with ephemeral port");
  len = sizeof(got);
  CHECK(getsockname(a, (struct sockaddr *)&got, &len) == 0 &&
        got.sin_family == AF_INET && got.sin_port, "getsockname ephemeral port");
  CHECK(bind(b, (struct sockaddr *)&named, sizeof(named)) == 0,
        "specific local port bind");
  int c = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(c >= 0, "second UDP socket creation");
  if (c >= 0) {
    errno = 0;
    CHECK(bind(c, (struct sockaddr *)&named, sizeof(named)) < 0 &&
          errno == EADDRINUSE, "duplicate port returns EADDRINUSE");
    close(c);
  }
  CHECK(connect(a, (const struct sockaddr *)peer, sizeof(*peer)) == 0,
        "connected datagram peer");
  len = sizeof(got);
  CHECK(getpeername(a, (struct sockaddr *)&got, &len) == 0 &&
        got.sin_addr.s_addr == peer->sin_addr.s_addr &&
        got.sin_port == peer->sin_port, "getpeername reports UDP peer");
  char byte;
  errno = 0;
  CHECK(recv(b, &byte, 1, 0) < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK),
        "nonblocking empty receive returns EAGAIN");
out:
  if (a >= 0) close(a);
  if (b >= 0) close(b);
}

static void echo_tests(const struct sockaddr_in *peer) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0, "blocking echo socket");
  if (fd >= 0) {
    CHECK(echo_once(fd, peer, "phase6-sendto", 0, 0) == 0,
          "blocking sendto/recv echo");
    close(fd);
  }
  fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  CHECK(fd >= 0, "nonblocking echo socket");
  if (fd >= 0) {
    CHECK(echo_once(fd, peer, "phase6-poll", 0, 0) == 0,
          "nonblocking UDP echo with poll");
    close(fd);
  }
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  int ok = fd >= 0 && connect(fd, (const struct sockaddr *)peer, sizeof(*peer)) == 0;
  CHECK(ok && echo_once(fd, peer, "phase6-connected", 1, 0) == 0,
        "connected UDP send/recv echo");
  if (fd >= 0) close(fd);
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0 && echo_once(fd, peer, "phase6-msg", 0, 1) == 0,
        "sendmsg/recvmsg UDP echo");
  if (fd >= 0) close(fd);

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  int ep = epoll_create1(0);
  CHECK(fd >= 0 && ep >= 0, "epoll setup for UDP socket");
  if (fd >= 0 && ep >= 0) {
    struct epoll_event add = { .events = EPOLLIN, .data.fd = fd }, event;
    const char text[] = "phase6-epoll";
    ok = epoll_ctl(ep, EPOLL_CTL_ADD, fd, &add) == 0 &&
         sendto(fd, text, sizeof(text)-1, 0, (const struct sockaddr *)peer,
                sizeof(*peer)) == (ssize_t)(sizeof(text)-1) &&
         epoll_wait(ep, &event, 1, WAIT_MS) == 1 && (event.events & EPOLLIN);
    CHECK(ok, "UDP receive readiness through epoll");
  }
  if (ep >= 0) close(ep);
  if (fd >= 0) close(fd);

  int fds[4] = { -1, -1, -1, -1 };
  ok = 1;
  for (unsigned i = 0; i < 4; i++) {
    char text[32];
    fds[i] = socket(AF_INET, SOCK_DGRAM, 0);
    snprintf(text, sizeof(text), "phase6-concurrent-%u", i);
    if (fds[i] < 0 || echo_once(fds[i], peer, text, 0, 0) < 0) ok = 0;
  }
  CHECK(ok, "four independent UDP echo sockets");
  for (unsigned i = 0; i < 4; i++) if (fds[i] >= 0) close(fds[i]);
}

static int echo_server(unsigned short port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in local = addr4("0.0.0.0", port);
  if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
    perror("echo server");
    return 1;
  }
  printf("[NET TEST] Phase 6 UDP echo server on port %u\n", port);
  for (;;) {
    char buf[1472];
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &len);
    if (n >= 0 && sendto(fd, buf, (size_t)n, 0,
                         (struct sockaddr *)&peer, len) != n) perror("sendto");
    else if (n < 0) perror("recvfrom");
  }
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--echo-server")) {
    unsigned long p = argc >= 3 ? strtoul(argv[2], NULL, 10) : DEFAULT_PORT;
    return echo_server((unsigned short)p);
  }
  const char *host = argc >= 2 ? argv[1] : DEFAULT_HOST;
  unsigned long port = argc >= 3 ? strtoul(argv[2], NULL, 10) : DEFAULT_PORT;
  struct sockaddr_in peer = addr4(host, (unsigned short)port);
  printf("[NET TEST] Phase 6 START (echo peer %s:%lu)\n", host, port);
  local_abi(&peer);
  echo_tests(&peer);
  printf("[NET TEST] Phase 6 %s: %d passed, %d failed\n",
         failed ? "FAIL" : "PASS", passed, failed);
  return failed ? 1 : 0;
}
