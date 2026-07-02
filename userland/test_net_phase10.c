#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int passed, failed;
#define TEST(condition, name) do {                                      \
  if (condition) { printf("[PASS] %s\n", name); passed++; }            \
  else { printf("[FAIL] %s\n", name); failed++; }                      \
} while (0)

static int read_value(const char *path, char *buffer, size_t size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buffer, size - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buffer[n] = 0;
  return (int)n;
}

int main(void) {
  char value[128];
  TEST(read_value("/sys/class/net/eth0/address", value, sizeof(value)) > 0 &&
           strchr(value, ':'), "MAC visibility");
  TEST(read_value("/sys/class/net/eth0/operstate", value, sizeof(value)) > 0 &&
           !strcmp(value, "up\n"), "link-state visibility");
  TEST(read_value("/sys/class/net/eth0/ipv4_address", value,
                  sizeof(value)) > 0 && strchr(value, '.'),
       "IPv4 configuration visibility");
  TEST(read_value("/sys/class/net/eth0/statistics/interrupts", value,
                  sizeof(value)) > 0 && strtoull(value, NULL, 10) > 0,
       "RTL8139 interrupt counters");
  TEST(read_value("/sys/class/net/eth0/rx_queue_depth", value,
                  sizeof(value)) > 0 && strtoul(value, NULL, 10) < 64,
       "bounded RX queue depth");
  TEST(read_value("/sys/class/net/eth0/packet_buffers_in_use", value,
                  sizeof(value)) > 0 && strtoul(value, NULL, 10) < 64,
       "bounded packet-buffer use");

  int churn_ok = 1;
  for (int i = 0; i < 256; i++) {
    int type = i & 1 ? SOCK_DGRAM : SOCK_STREAM;
    int protocol = i & 1 ? 17 : 6;
    int fd = socket(AF_INET, type, protocol);
    if (fd < 0) {
      churn_ok = 0;
      break;
    }
    close(fd);
  }
  TEST(churn_ok, "concurrent-protocol socket close churn");

  int dns = socket(AF_INET, SOCK_DGRAM, 0);
  int dynamic_nonblock_ok = dns >= 0;
  if (dynamic_nonblock_ok) {
    struct sockaddr_in resolver;
    memset(&resolver, 0, sizeof(resolver));
    resolver.sin_family = AF_INET;
    resolver.sin_port = htons(53);
    inet_pton(AF_INET, "10.0.2.3", &resolver.sin_addr);
    dynamic_nonblock_ok = fcntl(dns, F_SETFL, O_NONBLOCK) == 0 &&
                          connect(dns, (struct sockaddr *)&resolver,
                                  sizeof(resolver)) == 0;
    char byte;
    errno = 0;
    dynamic_nonblock_ok = dynamic_nonblock_ok && recv(dns, &byte, 1, 0) < 0 &&
                          (errno == EAGAIN || errno == EWOULDBLOCK);
    close(dns);
  }
  TEST(dynamic_nonblock_ok, "fcntl enables nonblocking AF_INET receive");

  printf("[NET TEST] Phase 10 %s: %d passed, %d failed\n",
         failed ? "FAIL" : "PASS", passed, failed);
  return failed ? 1 : 0;
}
