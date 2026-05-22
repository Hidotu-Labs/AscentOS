/*
 * Intel e1000 (82540EM) Network Interface Controller Driver
 *
 * A MMIO-based driver for the Intel PRO/1000 (e1000) NIC.
 * Uses memory-mapped BAR0 for register access and DMA for TX/RX descriptor
 * rings. This is the NIC emulated by QEMU's "-device e1000".
 *
 * Key differences from RTL8139:
 *   - MMIO instead of PIO for register access
 *   - Proper descriptor ring buffers (not a flat ring)
 *   - 64-bit DMA address support in descriptors
 */

#include "drivers/net/e1000.h"
#include "acpi/acpi.h"
#include "apic/ioapic.h"
#include "apic/lapic.h"
#include "console/console.h"
#include "console/klog.h"
#include "cpu/irq.h"
#include "cpu/isr.h"
#include "drivers/manager/device.h"
#include "drivers/pci/pci.h"
#include "io/io.h"
#include "lib/string.h"
#include "mm/dma_alloc.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/net.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── Intel e1000 PCI IDs ─────────────────────────────────────────────────────
#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E // 82540EM — the QEMU default

// ── Register Offsets (MMIO) ─────────────────────────────────────────────────
#define E1000_CTRL 0x00000   // Device Control
#define E1000_STATUS 0x00008 // Device Status
#define E1000_EECD 0x00010   // EEPROM/Flash Control
#define E1000_EERD 0x00014   // EEPROM Read
#define E1000_ICR 0x000C0    // Interrupt Cause Read
#define E1000_ICS 0x000C8    // Interrupt Cause Set
#define E1000_IMS 0x000D0    // Interrupt Mask Set
#define E1000_IMC 0x000D8    // Interrupt Mask Clear
#define E1000_RCTL 0x00100   // Receive Control
#define E1000_TCTL 0x00400   // Transmit Control
#define E1000_TIPG 0x00410   // Transmit Inter-Packet Gap
#define E1000_RDBAL 0x02800  // RX Descriptor Base Low
#define E1000_RDBAH 0x02804  // RX Descriptor Base High
#define E1000_RDLEN 0x02808  // RX Descriptor Length
#define E1000_RDH 0x02810    // RX Descriptor Head
#define E1000_RDT 0x02818    // RX Descriptor Tail
#define E1000_TDBAL 0x03800  // TX Descriptor Base Low
#define E1000_TDBAH 0x03804  // TX Descriptor Base High
#define E1000_TDLEN 0x03808  // TX Descriptor Length
#define E1000_TDH 0x03810    // TX Descriptor Head
#define E1000_TDT 0x03818    // TX Descriptor Tail
#define E1000_MTA 0x05200    // Multicast Table Array (128 × 32-bit)
#define E1000_RAL0 0x05400   // Receive Address Low  (MAC bytes 0-3)
#define E1000_RAH0 0x05404   // Receive Address High (MAC bytes 4-5 + AV)

// ── CTRL Register bits ──────────────────────────────────────────────────────
#define CTRL_SLU (1u << 6)  // Set Link Up
#define CTRL_ASDE (1u << 5) // Auto-Speed Detection Enable
#define CTRL_RST (1u << 26) // Device Reset

// ── STATUS Register bits ────────────────────────────────────────────────────
#define STATUS_LU (1u << 1) // Link Up

// ── EERD Register bits ──────────────────────────────────────────────────────
#define EERD_START (1u << 0) // Start Read
#define EERD_DONE (1u << 4)  // Read Done

// ── RCTL Register bits ──────────────────────────────────────────────────────
#define RCTL_EN (1u << 1)          // Receiver Enable
#define RCTL_SBP (1u << 2)         // Store Bad Packets
#define RCTL_UPE (1u << 3)         // Unicast Promiscuous Enable
#define RCTL_MPE (1u << 4)         // Multicast Promiscuous Enable
#define RCTL_LBM_NONE (0u << 6)    // No Loopback
#define RCTL_RDMTS_HALF (0u << 8)  // RX Desc Min Threshold = 1/2
#define RCTL_BAM (1u << 15)        // Broadcast Accept Mode
#define RCTL_BSIZE_2048 (0u << 16) // Buffer Size = 2048
#define RCTL_BSIZE_4096 (3u << 16) // with BSEX
#define RCTL_BSEX (1u << 25)       // Buffer Size Extension
#define RCTL_SECRC (1u << 26)      // Strip Ethernet CRC

// ── TCTL Register bits ──────────────────────────────────────────────────────
#define TCTL_EN (1u << 1)  // Transmit Enable
#define TCTL_PSP (1u << 3) // Pad Short Packets
#define TCTL_CT_SHIFT 4    // Collision Threshold
#define TCTL_COLD_SHIFT 12 // Collision Distance

// ── Interrupt bits (ICR/IMS/IMC) ────────────────────────────────────────────
#define ICR_TXDW (1u << 0)   // TX Descriptor Written Back
#define ICR_TXQE (1u << 1)   // TX Queue Empty
#define ICR_LSC (1u << 2)    // Link Status Change
#define ICR_RXSEQ (1u << 3)  // RX Sequence Error
#define ICR_RXDMT0 (1u << 4) // RX Desc Min Threshold Reached
#define ICR_RXO (1u << 6)    // RX Overrun
#define ICR_RXT0 (1u << 7)   // RX Timer Interrupt

// ── RX Descriptor (legacy format) ───────────────────────────────────────────
struct e1000_rx_desc {
  uint64_t addr;     // Buffer physical address
  uint16_t length;   // Received packet length
  uint16_t checksum; // Packet checksum
  uint8_t status;    // Descriptor status
  uint8_t errors;    // Descriptor errors
  uint16_t special;  // VLAN tag
} __attribute__((packed));

#define RXD_STAT_DD (1u << 0)  // Descriptor Done
#define RXD_STAT_EOP (1u << 1) // End of Packet

// ── TX Descriptor (legacy format) ───────────────────────────────────────────
struct e1000_tx_desc {
  uint64_t addr;    // Buffer physical address
  uint16_t length;  // Data length
  uint8_t cso;      // Checksum offset
  uint8_t cmd;      // Command field
  uint8_t status;   // Descriptor status
  uint8_t css;      // Checksum start
  uint16_t special; // VLAN tag
} __attribute__((packed));

#define TXD_CMD_EOP (1u << 0)  // End of Packet
#define TXD_CMD_IFCS (1u << 1) // Insert FCS/CRC
#define TXD_CMD_RS (1u << 3)   // Report Status
#define TXD_STAT_DD (1u << 0)  // Descriptor Done

// ── Ring sizes ──────────────────────────────────────────────────────────────
#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 8
#define E1000_RX_BUF_SIZE 2048

// ── Driver state ────────────────────────────────────────────────────────────

static bool nic_present = false;
static volatile uint8_t *mmio_base = NULL; // MMIO virtual address
static uint8_t nic_irq = 0;
static uint8_t nic_mac[6] = {0};

// RX descriptor ring
static struct e1000_rx_desc *rx_descs = NULL;  // Virtual address of ring
static uint64_t rx_descs_phys = 0;             // Physical address of ring
static uint8_t *rx_buffers[E1000_NUM_RX_DESC]; // Per-descriptor packet buffers
static uint64_t rx_buf_phys[E1000_NUM_RX_DESC];
static uint16_t rx_cur = 0;

// TX descriptor ring
static struct e1000_tx_desc *tx_descs = NULL;
static uint64_t tx_descs_phys = 0;
static uint8_t *tx_buffers[E1000_NUM_TX_DESC];
static uint64_t tx_buf_phys[E1000_NUM_TX_DESC];
static uint16_t tx_cur = 0;

// ── Statistics ──────────────────────────────────────────────────────────────
static uint64_t stat_rx_packets = 0;
static uint64_t stat_tx_packets = 0;
static uint64_t stat_rx_errors = 0;
static uint64_t stat_tx_errors = 0;

// ── Helpers ─────────────────────────────────────────────────────────────────

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

static void print_hex64(uint64_t val) {
  print_hex32((uint32_t)(val >> 32));
  print_hex32((uint32_t)(val & 0xFFFFFFFF));
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

// ── MMIO Register Access ────────────────────────────────────────────────────

static inline void e1000_write(uint32_t reg, uint32_t value) {
  *(volatile uint32_t *)(mmio_base + reg) = value;
}

static inline uint32_t e1000_read(uint32_t reg) {
  return *(volatile uint32_t *)(mmio_base + reg);
}

// ── EEPROM Read ─────────────────────────────────────────────────────────────

static uint16_t e1000_eeprom_read(uint8_t addr) {
  e1000_write(E1000_EERD, ((uint32_t)addr << 8) | EERD_START);

  uint32_t timeout = 1000000;
  uint32_t val;
  do {
    val = e1000_read(E1000_EERD);
    if (val & EERD_DONE) {
      return (uint16_t)(val >> 16);
    }
    timeout--;
  } while (timeout > 0);

  // Timeout — return 0
  return 0;
}

// ── Read MAC address ────────────────────────────────────────────────────────

static bool e1000_read_mac(void) {
  // Try EEPROM first
  uint16_t word0 = e1000_eeprom_read(0);
  uint16_t word1 = e1000_eeprom_read(1);
  uint16_t word2 = e1000_eeprom_read(2);

  if (word0 != 0 || word1 != 0 || word2 != 0) {
    nic_mac[0] = (uint8_t)(word0 & 0xFF);
    nic_mac[1] = (uint8_t)(word0 >> 8);
    nic_mac[2] = (uint8_t)(word1 & 0xFF);
    nic_mac[3] = (uint8_t)(word1 >> 8);
    nic_mac[4] = (uint8_t)(word2 & 0xFF);
    nic_mac[5] = (uint8_t)(word2 >> 8);
    return true;
  }

  // Fall back: read from RAL/RAH registers (pre-loaded by hardware)
  uint32_t ral = e1000_read(E1000_RAL0);
  uint32_t rah = e1000_read(E1000_RAH0);

  if (ral == 0 && (rah & 0xFFFF) == 0) {
    return false;
  }

  nic_mac[0] = (uint8_t)(ral & 0xFF);
  nic_mac[1] = (uint8_t)((ral >> 8) & 0xFF);
  nic_mac[2] = (uint8_t)((ral >> 16) & 0xFF);
  nic_mac[3] = (uint8_t)((ral >> 24) & 0xFF);
  nic_mac[4] = (uint8_t)(rah & 0xFF);
  nic_mac[5] = (uint8_t)((rah >> 8) & 0xFF);
  return true;
}

// ── RX Ring Initialization ──────────────────────────────────────────────────

static bool e1000_init_rx(void) {
  // Allocate descriptor ring using DMA allocator (uncached, below 4GB)
  dma_buffer_t *ring_buf = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT | DMA_FLAG_LOW);
  if (!ring_buf)
    return false;

  rx_descs_phys = ring_buf->phys;
  rx_descs = (struct e1000_rx_desc *)ring_buf->virt;
  memset(rx_descs, 0, PAGE_SIZE);

  // Allocate per-descriptor packet buffers using DMA allocator
  for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
    dma_buffer_t *pkt_buf = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT);
    if (!pkt_buf)
      return false;

    rx_buf_phys[i] = pkt_buf->phys;
    rx_buffers[i] = (uint8_t *)pkt_buf->virt;
    memset(rx_buffers[i], 0, PAGE_SIZE);

    rx_descs[i].addr = rx_buf_phys[i];
    rx_descs[i].status = 0;
  }

  // Program the hardware
  e1000_write(E1000_RDBAL, (uint32_t)(rx_descs_phys & 0xFFFFFFFF));
  e1000_write(E1000_RDBAH, (uint32_t)(rx_descs_phys >> 32));
  e1000_write(E1000_RDLEN,
              (uint32_t)(sizeof(struct e1000_rx_desc) * E1000_NUM_RX_DESC));
  e1000_write(E1000_RDH, 0);
  e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);

  rx_cur = 0;

  // Enable receiver
  e1000_write(E1000_RCTL,
              RCTL_EN | RCTL_BAM |  // Accept broadcast
                  RCTL_BSIZE_2048 | // 2048-byte buffers
                  RCTL_SECRC        // Strip CRC
  );

  return true;
}

// ── TX Ring Initialization ──────────────────────────────────────────────────

static bool e1000_init_tx(void) {
  // Allocate descriptor ring using DMA allocator (uncached, below 4GB)
  dma_buffer_t *ring_buf = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT | DMA_FLAG_LOW);
  if (!ring_buf)
    return false;

  tx_descs_phys = ring_buf->phys;
  tx_descs = (struct e1000_tx_desc *)ring_buf->virt;
  memset(tx_descs, 0, PAGE_SIZE);

  for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
    dma_buffer_t *pkt_buf = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT);
    if (!pkt_buf)
      return false;

    tx_buf_phys[i] = pkt_buf->phys;
    tx_buffers[i] = (uint8_t *)pkt_buf->virt;
    memset(tx_buffers[i], 0, PAGE_SIZE);

    tx_descs[i].addr = tx_buf_phys[i];
    tx_descs[i].status = TXD_STAT_DD; // Mark as "done" so first send works
    tx_descs[i].cmd = 0;
  }

  // Program the hardware
  e1000_write(E1000_TDBAL, (uint32_t)(tx_descs_phys & 0xFFFFFFFF));
  e1000_write(E1000_TDBAH, (uint32_t)(tx_descs_phys >> 32));
  e1000_write(E1000_TDLEN,
              (uint32_t)(sizeof(struct e1000_tx_desc) * E1000_NUM_TX_DESC));
  e1000_write(E1000_TDH, 0);
  e1000_write(E1000_TDT, 0);

  tx_cur = 0;

  // Configure transmit inter-packet gap (recommended values from Intel manual)
  // IPGT=10, IPGR1=10, IPGR2=10 for IEEE 802.3
  e1000_write(E1000_TIPG, 10 | (10 << 10) | (10 << 20));

  // Enable transmitter
  e1000_write(E1000_TCTL,
              TCTL_EN | TCTL_PSP |         // Pad short packets
                  (15u << TCTL_CT_SHIFT) | // Collision threshold
                  (64u << TCTL_COLD_SHIFT) // Collision distance (full duplex)
  );

  return true;
}

// ── IRQ Handler ─────────────────────────────────────────────────────────────

static void e1000_handle_rx(void) {
  while (rx_descs[rx_cur].status & RXD_STAT_DD) {
    uint16_t pkt_len = rx_descs[rx_cur].length;

    if (pkt_len > 0 && pkt_len <= ETH_FRAME_MAX &&
        (rx_descs[rx_cur].status & RXD_STAT_EOP)) {
      // Complete packet — enqueue it
      net_rx_enqueue(rx_buffers[rx_cur], pkt_len);
      stat_rx_packets++;
    } else if (rx_descs[rx_cur].errors) {
      stat_rx_errors++;
    }

    // Reset descriptor for reuse
    rx_descs[rx_cur].status = 0;

    uint16_t old_cur = rx_cur;
    rx_cur = (rx_cur + 1) % E1000_NUM_RX_DESC;

    // Advance tail to return this descriptor to hardware
    e1000_write(E1000_RDT, old_cur);
  }
}

static void e1000_irq_handler(struct registers *regs) {
  (void)regs;

  // Read and acknowledge all pending interrupt causes
  uint32_t icr = e1000_read(E1000_ICR);

  if (icr == 0)
    return; // Spurious

  if (icr & ICR_RXT0) {
    e1000_handle_rx();
  }

  if (icr & ICR_RXDMT0) {
    e1000_handle_rx();
  }

  if (icr & ICR_RXO) {
    // RX overrun — still try to drain what we can
    stat_rx_errors++;
    e1000_handle_rx();
  }

  if (icr & ICR_TXDW) {
    stat_tx_packets++;
  }

  if (icr & ICR_LSC) {
    uint32_t status = e1000_read(E1000_STATUS);
    console_puts("[E1000] Link status changed: ");
    console_puts((status & STATUS_LU) ? "UP\n" : "DOWN\n");
  }
}

// ── Public API ──────────────────────────────────────────────────────────────

static int e1000_probe(struct device *dev) {
  // Use resources from 'dev' instead of global scanning
  // For now, we'll keep using the existing logic but wrapped in match

  console_puts("[E1000] Probing device ");
  console_puts(dev->name);
  console_puts("\n");

  // Step 2: Map the MMIO region from BAR0
  // Note: resources[0] should be BAR0 MMIO
  if (dev->resource_count == 0 || dev->resources[0].type != RES_MEM) {
    return -1;
  }

  uint64_t bar0_phys = dev->resources[0].start;

  // Map 128KB of MMIO space as UNCACHED (PCD|PWT)
#define E1000_MMIO_SIZE (128 * 1024)
  uint64_t mmio_virt = bar0_phys + pmm_get_hhdm_offset();
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t mmio_flags =
      PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_PCD | PAGE_FLAG_PWT;

  for (uint64_t offset = 0; offset < E1000_MMIO_SIZE; offset += PAGE_SIZE) {
    vmm_map_page(pml4, mmio_virt + offset, bar0_phys + offset, mmio_flags);
  }
  vmm_flush_tlb(mmio_virt);

  mmio_base = (volatile uint8_t *)mmio_virt;

  // Find IRQ resource
  for (size_t i = 0; i < dev->resource_count; i++) {
    if (dev->resources[i].type == RES_IRQ) {
      nic_irq = (uint8_t)dev->resources[i].start;
      break;
    }
  }

  // ── Step 3: Enable PCI bus mastering (required for DMA) ─────────────
  // We need the bus/slot/func. We can store them in dev->driver_data
  // or use a helper. For now, we'll just scan the legacy PCI data using
  // vendor/device.
  struct pci_device *pci_dev =
      pci_find_device_by_id(dev->vendor_id, dev->device_id);
  if (!pci_dev)
    return -1;

  pci_enable_bus_mastering(pci_dev);

  // Also enable memory space access in the PCI command register
  uint32_t cmd =
      pci_config_read32(pci_dev->bus, pci_dev->slot, pci_dev->func, 0x04);
  cmd |= (1 << 1);   // Memory Space Enable
  cmd &= ~(1 << 10); // ENABLE interrupts (clear Interrupt Disable bit)
  pci_config_write16(pci_dev->bus, pci_dev->slot, pci_dev->func, 0x04,
                     (uint16_t)cmd);

  // ── Step 4: Software reset ──────────────────────────────────────────
  uint32_t ctrl = e1000_read(E1000_CTRL);
  e1000_write(E1000_CTRL, ctrl | CTRL_RST);

  // Wait for reset to complete (hardware clears the RST bit)
  uint32_t timeout = 1000000;
  while ((e1000_read(E1000_CTRL) & CTRL_RST) && timeout > 0) {
    timeout--;
  }
  if (timeout == 0) {
    console_puts("[ERR] e1000 reset timed out!\n");
    return -1;
  }

  // Small delay after reset
  for (volatile int i = 0; i < 100000; i++) {
  }

  console_puts("     Software reset complete.\n");

  // ── Step 5: Disable all interrupts during setup ─────────────────────
  e1000_write(E1000_IMC, 0xFFFFFFFF);

  // ── Step 6: Set link up ─────────────────────────────────────────────
  ctrl = e1000_read(E1000_CTRL);
  ctrl |= CTRL_SLU;  // Set Link Up
  ctrl |= CTRL_ASDE; // Auto-Speed Detection
  ctrl &= ~CTRL_RST; // Clear reset bit
  e1000_write(E1000_CTRL, ctrl);

  // ── Step 7: Clear multicast table array ─────────────────────────────
  for (int i = 0; i < 128; i++) {
    e1000_write(E1000_MTA + (i * 4), 0);
  }

  // ── Step 8: Read MAC address ────────────────────────────────────────
  if (!e1000_read_mac()) {
    console_puts("[ERR] e1000: failed to read MAC address.\n");
    return -1;
  }

  // Program MAC into RAL0/RAH0 (Address Valid bit in RAH)
  uint32_t ral = (uint32_t)nic_mac[0] | ((uint32_t)nic_mac[1] << 8) |
                 ((uint32_t)nic_mac[2] << 16) | ((uint32_t)nic_mac[3] << 24);
  uint32_t rah = (uint32_t)nic_mac[4] | ((uint32_t)nic_mac[5] << 8) |
                 (1u << 31); // AV = Address Valid
  e1000_write(E1000_RAL0, ral);
  e1000_write(E1000_RAH0, rah);

  console_puts("     MAC: ");
  for (int i = 0; i < 6; i++) {
    print_hex8(nic_mac[i]);
    if (i < 5)
      console_putchar(':');
  }
  console_putchar('\n');

  // ── Step 9: Initialize RX ring ──────────────────────────────────────
  if (!e1000_init_rx()) {
    console_puts("[ERR] e1000: failed to initialize RX ring.\n");
    return -1;
  }
  console_puts("     RX ring initialized (");
  print_uint32(E1000_NUM_RX_DESC);
  console_puts(" descriptors).\n");

  // ── Step 10: Initialize TX ring ─────────────────────────────────────
  if (!e1000_init_tx()) {
    console_puts("[ERR] e1000: failed to initialize TX ring.\n");
    return -1;
  }
  console_puts("     TX ring initialized (");
  print_uint32(E1000_NUM_TX_DESC);
  console_puts(" descriptors).\n");

  irq_install_handler(nic_irq, e1000_irq_handler, 0x000F);

  // ── Step 12: Enable interrupts ──────────────────────────────────────
  e1000_write(E1000_IMS,
              ICR_RXT0 |       // RX Timer
                  ICR_RXDMT0 | // RX Desc Min Threshold
                  ICR_RXO |    // RX Overrun
                  ICR_TXDW |   // TX Descriptor Written Back
                  ICR_LSC      // Link Status Change
  );

  // Read ICR to clear any pending interrupts from reset
  (void)e1000_read(E1000_ICR);

  nic_present = true;

  // Print link status
  uint32_t status = e1000_read(E1000_STATUS);
  console_puts("     Link: ");
  console_puts((status & STATUS_LU) ? "UP\n" : "DOWN\n");

  console_puts("[OK] Intel e1000 driver initialized.\n\n");
  return 0; // Success
}

static struct device_id e1000_ids[] = {
    {.type = ID_PCI,
     .pci = {.vendor = E1000_VENDOR_ID, .device = E1000_DEVICE_ID}}};

static struct driver e1000_driver = {
    .name = "e1000", .ids = e1000_ids, .id_count = 1, .probe = e1000_probe};

void e1000_init(void) { dm_register_driver(&e1000_driver); }

const uint8_t *e1000_get_mac(void) {
  if (!nic_present)
    return NULL;
  return nic_mac;
}

bool e1000_link_up(void) {
  if (!nic_present)
    return false;
  uint32_t status = e1000_read(E1000_STATUS);
  return (status & STATUS_LU) != 0;
}

bool e1000_is_present(void) { return nic_present; }

int e1000_send(const void *data, uint16_t len) {
  if (!nic_present)
    return -1;
  if (len > ETH_FRAME_MAX)
    return -1;

  // Wait for the current descriptor to be free
  uint32_t timeout = 1000000;
  while (!(tx_descs[tx_cur].status & TXD_STAT_DD) && timeout > 0) {
    timeout--;
  }
  if (timeout == 0) {
    stat_tx_errors++;
    return -1;
  }

  // Copy data into the TX buffer
  memcpy(tx_buffers[tx_cur], data, len);

  // Setup the descriptor
  tx_descs[tx_cur].addr = tx_buf_phys[tx_cur];
  tx_descs[tx_cur].length = len;
  tx_descs[tx_cur].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
  tx_descs[tx_cur].status = 0;

  // Advance tail pointer — tells the NIC to transmit
  uint16_t old_cur = tx_cur;
  tx_cur = (tx_cur + 1) % E1000_NUM_TX_DESC;
  e1000_write(E1000_TDT, tx_cur);

  (void)old_cur;
  return 0;
}

bool e1000_poll(void) {
  if (!nic_present)
    return false;

  // Check for pending RX packets directly
  bool found = false;

  while (rx_descs[rx_cur].status & RXD_STAT_DD) {
    uint16_t pkt_len = rx_descs[rx_cur].length;

    if (pkt_len > 0 && pkt_len <= ETH_FRAME_MAX &&
        (rx_descs[rx_cur].status & RXD_STAT_EOP)) {
      net_rx_enqueue(rx_buffers[rx_cur], pkt_len);
      stat_rx_packets++;
      found = true;
    } else if (rx_descs[rx_cur].errors) {
      stat_rx_errors++;
    }

    rx_descs[rx_cur].status = 0;

    uint16_t old_cur = rx_cur;
    rx_cur = (rx_cur + 1) % E1000_NUM_RX_DESC;
    e1000_write(E1000_RDT, old_cur);
  }

  return found;
}
