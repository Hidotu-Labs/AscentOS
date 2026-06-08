/*
 * RTL8139 Network Interface Controller Driver
 *
 * A simple PIO-based driver for the Realtek 8139 NIC.
 * Uses port I/O (BAR0) for register access and DMA for TX/RX buffers.
 */

#include "drivers/net/rtl8139.h"
#include "acpi/acpi.h"
#include "apic/ioapic.h"
#include "apic/lapic.h"
#include "console/console.h"
#include "console/klog.h"
#include "cpu/isr.h"
#include "cpu/irq.h"
#include "drivers/pci/pci.h"
#include "io/io.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/net.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// RTL8139 Register Offsets (PIO)
#define RTL_IDR0 0x00 // MAC address byte 0
#define RTL_IDR1 0x01
#define RTL_IDR2 0x02
#define RTL_IDR3 0x03
#define RTL_IDR4 0x04
#define RTL_IDR5 0x05
#define RTL_MAR0 0x08    // Multicast filter registers (8 bytes)
#define RTL_TSD0 0x10    // TX status descriptor 0  (4 descriptors, +4 each)
#define RTL_TSAD0 0x20   // TX start address descriptor 0 (+4 each)
#define RTL_RBSTART 0x30 // RX buffer start address (physical)
#define RTL_CR 0x37      // Command register
#define RTL_CAPR 0x38    // Current Address of Packet Read
#define RTL_CBR 0x3A     // Current Buffer Address
#define RTL_IMR 0x3C     // Interrupt Mask Register
#define RTL_ISR 0x3E     // Interrupt Status Register
#define RTL_TCR 0x40     // TX Configuration Register
#define RTL_RCR 0x44     // RX Configuration Register
#define RTL_MPC 0x4C     // Missed Packet Counter
#define RTL_CONFIG1 0x52 // Configuration Register 1
#define RTL_MSR 0x58     // Media Status Register
#define RTL_BMCR 0x62    // Basic Mode Control Register

// Command Register (CR) bits
#define CR_RST 0x10  // Reset
#define CR_RE 0x08   // Receiver Enable
#define CR_TE 0x04   // Transmitter Enable
#define CR_BUFE 0x01 // Buffer Empty (RX)

// Interrupt Status/Mask bits
#define INT_ROK (1 << 0)      // Receive OK
#define INT_RER (1 << 1)      // Receive Error
#define INT_TOK (1 << 2)      // Transmit OK
#define INT_TER (1 << 3)      // Transmit Error
#define INT_RXOVW (1 << 4)    // RX Buffer Overflow
#define INT_LNKCHG (1 << 5)   // Link Change
#define INT_TIMEOUT (1 << 14) // Timeout

// TX Status Descriptor bits
#define TSD_OWN (1 << 13)    // DMA ownership (0 = NIC owns, 1 = driver owns)
#define TSD_TOK (1 << 15)    // Transmit OK
#define TSD_SIZE_MASK 0x1FFF // Packet size (bits 0-12)

// RX Configuration Register bits
#define RCR_AAP (1 << 0)  // Accept All Packets (promiscuous)
#define RCR_APM (1 << 1)  // Accept Physical Match
#define RCR_AM (1 << 2)   // Accept Multicast
#define RCR_AB (1 << 3)   // Accept Broadcast
#define RCR_WRAP (1 << 7) // Wrap bit — receive buffer wraps around

// TX Configuration Register bits
#define TCR_IFG96 (3 << 24)     // Inter-frame gap = 96-bit times (standard)
#define TCR_MXDMA_2048 (7 << 8) // Max DMA burst = 2048 bytes

// Media Status Register bits
#define MSR_LINK                                                               \
  (1 << 2) // Link status (0 = link up, 1 = link down — inverted!)

// Buffer sizes
#define RX_BUF_SIZE 8192
#define RX_BUF_PAD 16    // RTL8139 hardware requires 16-byte pad
#define RX_BUF_WRAP 1536 // Extra space for wrap-around
#define RX_BUF_TOTAL (RX_BUF_SIZE + RX_BUF_PAD + RX_BUF_WRAP)

#define TX_BUF_SIZE 2048 // Each TX buffer
#define TX_DESC_COUNT 4  // RTL8139 has exactly 4 TX descriptors

#define RTL_VENDOR_ID 0x10EC
#define RTL_DEVICE_ID 0x8139

// Driver state

static bool nic_present = false;
static uint16_t nic_iobase = 0;
static uint8_t nic_irq = 0;
static uint8_t nic_mac[6] = {0};

// RX ring buffer
static uint8_t *rx_buffer = NULL;   // Virtual address
static uint64_t rx_buffer_phys = 0; // Physical address for DMA
static uint32_t rx_cur_offset = 0;  // Current read offset in the ring

// TX descriptors
static uint8_t *tx_buffers[TX_DESC_COUNT] = {0};      // Virtual addresses
static uint64_t tx_buffers_phys[TX_DESC_COUNT] = {0}; // Physical addresses
static uint8_t tx_cur_desc = 0; // Current TX descriptor index

// Statistics
static uint64_t stat_rx_packets = 0;
static uint64_t stat_tx_packets = 0;
static uint64_t stat_rx_errors = 0;
static uint64_t stat_tx_errors = 0;

// Helpers

static void print_hex8(uint8_t val) {
  const char *hex = "0123456789ABCDEF";
  console_putchar(hex[(val >> 4) & 0xF]);
  console_putchar(hex[val & 0xF]);
}

static void print_hex16(uint16_t val) {
  print_hex8((uint8_t)(val >> 8));
  print_hex8((uint8_t)(val & 0xFF));
}

static void print_hex32(uint32_t val) {
  print_hex16((uint16_t)(val >> 16));
  print_hex16((uint16_t)(val & 0xFFFF));
}

static void print_uint32(uint32_t num) {
  if (num == 0) {
    console_putchar('0');
    return;
  }
  char buf[10];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    console_putchar(buf[--i]);
  }
}

// IRQ Handler

static void rtl8139_irq_handler(struct registers *regs);

static void rtl8139_irq_handler(struct registers *regs) {
  (void)regs;

  uint16_t status = inw(nic_iobase + RTL_ISR);

  if (status == 0)
    return; // Spurious

  // Acknowledge immediately so we don't re-enter
  outw(nic_iobase + RTL_ISR, status);

  if (status & INT_ROK) {
    // Process received packets from the ring buffer
    while (!(inb(nic_iobase + RTL_CR) & CR_BUFE)) {
      uint32_t offset = rx_cur_offset % RX_BUF_SIZE;
      uint16_t rx_status = *(uint16_t *)(rx_buffer + offset);
      uint16_t rx_length = *(uint16_t *)(rx_buffer + offset + 2);

      // Validate: ROK bit must be set, length must be sane
      if (!(rx_status & 0x0001) || rx_length < 8 || rx_length > 1536) {
        stat_rx_errors++;
        rx_cur_offset = inw(nic_iobase + RTL_CBR) % RX_BUF_SIZE;
        outw(nic_iobase + RTL_CAPR, (uint16_t)(rx_cur_offset - 16));
        break;
      }

      uint16_t pkt_len = rx_length - 4; // Strip CRC
      if (pkt_len > 0 && pkt_len <= ETH_FRAME_MAX) {
        net_rx_enqueue(rx_buffer + offset + 4, pkt_len);
      }
      stat_rx_packets++;

      // Advance read pointer: 4-byte header + rx_length, aligned to 4 bytes
      rx_cur_offset = (offset + rx_length + 4 + 3) & ~3u;
      outw(nic_iobase + RTL_CAPR, (uint16_t)((rx_cur_offset - 16) & 0xFFFF));
    }
  }

  if (status & INT_TOK) {
    stat_tx_packets++;
  }

  if (status & INT_RER) {
    stat_rx_errors++;
  }

  if (status & INT_TER) {
    stat_tx_errors++;
  }

  if (status & INT_RXOVW) {
    stat_rx_errors++;
    rx_cur_offset = inw(nic_iobase + RTL_CBR) % RX_BUF_SIZE;
    outw(nic_iobase + RTL_CAPR, (uint16_t)(rx_cur_offset - 16));
  }
}

// Public API

void rtl8139_init(void) {
  // Step 1: Find the NIC on the PCI bus
  struct pci_device *dev = pci_find_device_by_id(RTL_VENDOR_ID, RTL_DEVICE_ID);
  if (!dev) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " RTL8139 NIC not found on PCI bus.\n");
    return;
  }

  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " RTL8139 NIC found at PCI ");
  print_uint32(dev->bus);
  console_putchar(':');
  print_uint32(dev->slot);
  console_putchar('.');
  print_uint32(dev->func);
  console_putchar('\n');

  // Step 2: Read I/O base from BAR0
  // BAR0 bit 0 = 1 means I/O space; bits [31:2] are the I/O base
  uint32_t bar0 = dev->bar[0];
  if (!(bar0 & 1)) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " RTL8139 BAR0 is memory-mapped, expected I/O.\n");
    return;
  }
  nic_iobase = (uint16_t)(bar0 & 0xFFFC);
  nic_irq = dev->irq_line;

  console_puts("     I/O Base: 0x");
  print_hex16(nic_iobase);
  console_puts("  IRQ: ");
  print_uint32(nic_irq);
  console_putchar('\n');

  // Step 3: Enable PCI bus mastering (required for DMA)
  pci_enable_bus_mastering(dev);

  // Step 4: Power on the NIC
  outb(nic_iobase + RTL_CONFIG1, 0x00);

  // Step 5: Software reset
  outb(nic_iobase + RTL_CR, CR_RST);

  // Spin until the reset bit clears (hardware clears it when done)
  uint32_t timeout = 100000;
  while ((inb(nic_iobase + RTL_CR) & CR_RST) && timeout > 0) {
    timeout--;
  }
  if (timeout == 0) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " RTL8139 reset timed out!\n");
    return;
  }
  console_puts("     Software reset complete.\n");

  // Step 6: Read MAC address
  for (int i = 0; i < 6; i++) {
    nic_mac[i] = inb(nic_iobase + RTL_IDR0 + i);
  }

  console_puts("     MAC: ");
  for (int i = 0; i < 6; i++) {
    print_hex8(nic_mac[i]);
    if (i < 5)
      console_putchar(':');
  }
  console_putchar('\n');

  // Step 7: Allocate RX buffer (physically contiguous)
  // Need RX_BUF_TOTAL bytes = ~9.7KB ≈ 3 pages
  uint32_t rx_pages = (RX_BUF_TOTAL + PAGE_SIZE - 1) / PAGE_SIZE;
  void *rx_phys = pmm_alloc_blocks(rx_pages);
  if (!rx_phys) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " RTL8139: failed to allocate RX buffer.\n");
    return;
  }
  rx_buffer_phys = (uint64_t)rx_phys;
  rx_buffer = (uint8_t *)(rx_buffer_phys + pmm_get_hhdm_offset());
  memset(rx_buffer, 0, RX_BUF_TOTAL);

  // Tell the NIC where to DMA received packets
  outl(nic_iobase + RTL_RBSTART, (uint32_t)(rx_buffer_phys & 0xFFFFFFFF));
  console_puts("     RX buffer at phys: 0x");
  print_hex32((uint32_t)rx_buffer_phys);
  console_putchar('\n');

  // Step 8: Allocate 4 TX buffers
  for (int i = 0; i < TX_DESC_COUNT; i++) {
    void *tx_phys = pmm_alloc();
    if (!tx_phys) {
      console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " RTL8139: failed to allocate TX buffer.\n");
      return;
    }
    tx_buffers_phys[i] = (uint64_t)tx_phys;
    tx_buffers[i] = (uint8_t *)(tx_buffers_phys[i] + pmm_get_hhdm_offset());
    memset(tx_buffers[i], 0, TX_BUF_SIZE);

    // Pre-load TX start address descriptors
    outl(nic_iobase + RTL_TSAD0 + (i * 4),
         (uint32_t)(tx_buffers_phys[i] & 0xFFFFFFFF));
  }
  console_puts("     TX descriptors initialized.\n");

  // Step 9: Configure interrupts
  // Enable ROK, TOK, RX error, TX error, and RX overflow interrupts
  outw(nic_iobase + RTL_IMR, INT_ROK | INT_TOK | INT_RER | INT_TER | INT_RXOVW);

  // Step 10: Configure RX
  // Accept broadcast + physical match + multicast, wrap, no FIFO threshold
  outl(nic_iobase + RTL_RCR,
       RCR_AB | RCR_APM | RCR_AM | RCR_WRAP |
           (7 << 8) |  // RBLEN = 0 (8K buffer), MXDMA = 7 (unlimited)
           (7 << 13)); // RX FIFO threshold: no threshold (transfer all)

  // Step 11: Configure TX
  outl(nic_iobase + RTL_TCR, TCR_IFG96 | TCR_MXDMA_2048);

  // Step 12: Install IRQ handler and route through I/O APIC
  irq_install_handler(nic_irq, rtl8139_irq_handler, 0x000F);

  console_puts("     IRQ ");
  print_uint32(nic_irq);
  console_puts(" -> Vector ");
  print_uint32(32 + nic_irq);
  console_putchar('\n');

  // Step 13: Enable RX and TX
  outb(nic_iobase + RTL_CR, CR_RE | CR_TE);

  rx_cur_offset = 0;
  tx_cur_desc = 0;
  nic_present = true;

  // Print link status
  uint8_t msr = inb(nic_iobase + RTL_MSR);
  console_puts("     Link: ");
  console_puts((msr & MSR_LINK) ? "DOWN\n" : "UP\n");

  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " RTL8139 driver initialized.\n\n");
}

const uint8_t *rtl8139_get_mac(void) {
  if (!nic_present)
    return NULL;
  return nic_mac;
}

bool rtl8139_link_up(void) {
  if (!nic_present)
    return false;
  uint8_t msr = inb(nic_iobase + RTL_MSR);
  return !(msr & MSR_LINK); // Bit is inverted: 0 = link up
}

bool rtl8139_is_present(void) { return nic_present; }

int rtl8139_send(const void *data, uint16_t len) {
  if (!nic_present)
    return -1;
  if (len > TX_BUF_SIZE)
    return -1;

  uint8_t desc = tx_cur_desc;

  // Wait for the current descriptor to be free (OWN bit set = driver owns it)
  uint32_t tsd = inl(nic_iobase + RTL_TSD0 + (desc * 4));
  uint32_t timeout = 100000;
  while (!(tsd & TSD_OWN) && timeout > 0) {
    tsd = inl(nic_iobase + RTL_TSD0 + (desc * 4));
    timeout--;
  }
  if (timeout == 0) {
    stat_tx_errors++;
    return -1;
  }

  // Copy data into the TX buffer
  memcpy(tx_buffers[desc], data, len);

  // Tell the NIC the size and start transmission
  // Writing to TSD clears OWN bit, which signals the NIC to transmit
  outl(nic_iobase + RTL_TSD0 + (desc * 4), (uint32_t)len);

  // Advance to next descriptor
  tx_cur_desc = (desc + 1) % TX_DESC_COUNT;

  return 0;
}

uint16_t rtl8139_get_iobase(void) { return nic_iobase; }

uint8_t rtl8139_get_irq(void) { return nic_irq; }

bool rtl8139_poll(void) {
  if (!nic_present)
    return false;

  // Check the RX ring directly — don't rely on ISR bits which may have
  // already been cleared by the IRQ handler.
  bool found = false;

  while (!(inb(nic_iobase + RTL_CR) & CR_BUFE)) {
    uint32_t offset = rx_cur_offset % RX_BUF_SIZE;

    // RTL8139 RX packet header: [status:16][length:16][data...]
    uint16_t rx_status = *(uint16_t *)(rx_buffer + offset);
    uint16_t rx_length = *(uint16_t *)(rx_buffer + offset + 2);

    // Validate: ROK bit must be set, length must be sane
    if (!(rx_status & 0x0001) || rx_length < 8 || rx_length > 1536) {
      stat_rx_errors++;
      // Resync to hardware's current position
      rx_cur_offset = inw(nic_iobase + RTL_CBR) % RX_BUF_SIZE;
      outw(nic_iobase + RTL_CAPR, (uint16_t)(rx_cur_offset - 16));
      break;
    }

    // Packet data starts at offset+4; length includes 4-byte CRC
    uint16_t pkt_len = rx_length - 4;
    if (pkt_len > 0 && pkt_len <= ETH_FRAME_MAX) {
      net_rx_enqueue(rx_buffer + offset + 4, pkt_len);
      found = true;
    }
    stat_rx_packets++;

    // Advance read pointer: 4-byte header + rx_length bytes, aligned to 4
    rx_cur_offset = (offset + rx_length + 4 + 3) & ~3u;

    // Update hardware CAPR (hardware quirk: must write offset - 16)
    uint16_t capr = (uint16_t)((rx_cur_offset - 16) & 0xFFFF);
    outw(nic_iobase + RTL_CAPR, capr);

    // Acknowledge ROK so the hardware knows we consumed this packet
    outw(nic_iobase + RTL_ISR, INT_ROK);
  }

  // Also drain any non-RX status bits (TX OK, errors) to keep ISR clean
  uint16_t status = inw(nic_iobase + RTL_ISR);
  if (status & ~INT_ROK) {
    if (status & INT_TOK)  stat_tx_packets++;
    if (status & INT_RER)  stat_rx_errors++;
    if (status & INT_TER)  stat_tx_errors++;
    if (status & INT_RXOVW) {
      stat_rx_errors++;
      rx_cur_offset = inw(nic_iobase + RTL_CBR) % RX_BUF_SIZE;
      outw(nic_iobase + RTL_CAPR, (uint16_t)(rx_cur_offset - 16));
    }
    outw(nic_iobase + RTL_ISR, status & ~INT_ROK);
  }

  return found;
}
