#include "serial.h"
#include "../io/io.h"

#define COM1 0x3F8

// Ring buffer for non-blocking serial output
#define SERIAL_BUF_SIZE 4096
static char serial_buf[SERIAL_BUF_SIZE];
static volatile uint32_t serial_head = 0; // write position
static volatile uint32_t serial_tail = 0; // read/send position

void serial_init(void) {
  outb(COM1 + 1, 0x00); // Disable all interrupts
  outb(COM1 + 3, 0x80); // Enable DLAB (set baud rate divisor)
  outb(COM1 + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
  outb(COM1 + 1, 0x00); //                  (hi byte)
  outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
  outb(COM1 + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
  outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

static int is_transmit_empty(void) { return inb(COM1 + 5) & 0x20; }

// Enqueue a character into the ring buffer (non-blocking, drops if full)
static void serial_enqueue(char c) {
  uint32_t next = (serial_head + 1) % SERIAL_BUF_SIZE;
  if (next == serial_tail)
    return; // buffer full — drop silently
  serial_buf[serial_head] = c;
  serial_head = next;
}

// Drain as many queued bytes as the UART FIFO can accept right now (non-blocking)
void serial_flush(void) {
  // THRE means the transmit FIFO is empty, so one status check is enough
  // before filling up to the 16550 FIFO's 16-byte capacity.
  if (!is_transmit_empty())
    return;

  int max_drain = 16;
  while (serial_tail != serial_head && max_drain-- > 0) {
    outb(COM1, serial_buf[serial_tail]);
    serial_tail = (serial_tail + 1) % SERIAL_BUF_SIZE;
  }
}

void serial_putchar(char c) {
  if (c == '\n')
    serial_enqueue('\r');
  serial_enqueue(c);
  // Opportunistic drain — send what we can without blocking
  serial_flush();
}

void serial_write(const char *data, size_t length) {
  if (!data)
    return;

  for (size_t i = 0; i < length; i++) {
    if (data[i] == '\n')
      serial_enqueue('\r');
    serial_enqueue(data[i]);
  }

  // Check/drain the UART once for the whole write instead of once per byte.
  serial_flush();
}

int serial_received(void) { return inb(COM1 + 5) & 1; }

char serial_get_char(void) {
  while (serial_received() == 0)
    ;
  return inb(COM1);
}
