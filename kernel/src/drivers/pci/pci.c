#include "drivers/pci/pci.h"
#include "console/console.h"
#include "console/klog.h"
#include "drivers/manager/device.h"
#include "drivers/pci/pcie.h"
#include "io/io.h"
#include "lib/string.h"
#include <stddef.h>
static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t device_count = 0;

// PCI Config Space Access

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  if (pcie_available(bus)) {
    return pcie_config_read32(bus, slot, func, offset);
  }
  if (offset > 255)
    return 0xFFFFFFFF;
  uint32_t address = (1u << 31) | ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(PCI_CONFIG_ADDRESS, address);
  return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint32_t value) {
  if (pcie_available(bus)) {
    pcie_config_write32(bus, slot, func, offset, value);
    return;
  }
  if (offset > 255)
    return;
  uint32_t address = (1u << 31) | ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(PCI_CONFIG_ADDRESS, address);
  outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset) {
  uint32_t val = pci_config_read32(bus, slot, func, offset & 0xFFFC);
  return (uint16_t)(val >> ((offset & 2) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint16_t value) {
  uint32_t val = pci_config_read32(bus, slot, func, offset & 0xFFFC);
  uint32_t mask = 0xFFFFu << ((offset & 2) * 8);
  val &= ~mask;
  val |= (uint32_t)value << ((offset & 2) * 8);
  pci_config_write32(bus, slot, func, offset & 0xFFFC, val);
}

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

// Device scanning

static void pci_check_function(uint8_t bus, uint8_t slot, uint8_t func) {
  uint32_t reg0 = pci_config_read32(bus, slot, func, 0x00);
  uint16_t vendor_id = reg0 & 0xFFFF;
  uint16_t device_id = reg0 >> 16;

  if (vendor_id == 0xFFFF)
    return; // No device
  if (device_count >= PCI_MAX_DEVICES)
    return;

  uint32_t reg2 = pci_config_read32(bus, slot, func, 0x08);
  uint32_t reg3 = pci_config_read32(bus, slot, func, 0x0C);
  uint32_t reg_irq = pci_config_read32(bus, slot, func, 0x3C);

  struct pci_device *dev = &devices[device_count];
  dev->bus = bus;
  dev->slot = slot;
  dev->func = func;
  dev->vendor_id = vendor_id;
  dev->device_id = device_id;
  dev->class_code = (reg2 >> 24) & 0xFF;
  dev->subclass = (reg2 >> 16) & 0xFF;
  dev->prog_if = (reg2 >> 8) & 0xFF;
  dev->header_type = (reg3 >> 16) & 0xFF;
  dev->irq_line = reg_irq & 0xFF;

  // Read BARs (only for standard header type 0x00)
  char dev_path[32];
  // Format: 00:02.0
  // Note: we are hardcoding seg0 for now
  strcpy(dev_path, "00:00.0"); // TODO: use actual bus/slot/func
  // We'll just use a simple name for now
  char dev_name[16];
  // Format: 00:02.0
  // Int to string is not available easily without snprintf
  // Let's just name them by index or something simple
  static int dev_idx = 0;
  char buf[4];
  buf[0] = '0' + (dev_idx / 10);
  buf[1] = '0' + (dev_idx % 10);
  buf[2] = '\0';
  strcpy(dev_name, "pci_dev");
  strcat(dev_name, buf);
  dev_idx++;

  struct device *seg_dev = device_find_by_path("/sys/pci/seg0");
  if (!seg_dev)
    seg_dev = device_find_by_path("/sys/pci"); // Fallback

  struct device *pci_node = device_create(seg_dev, dev_name);
  if (pci_node) {
    pci_node->vendor_id = vendor_id;
    pci_node->device_id = device_id;
    pci_node->pci_class = dev->class_code;
    pci_node->pci_subclass = dev->subclass;
    pci_node->pci_prog_if = dev->prog_if;
    dm_probe_device(pci_node);
  }

  if ((dev->header_type & 0x7F) == 0x00) {
    for (int i = 0; i < 6; i++) {
      uint32_t bar = pci_config_read32(bus, slot, func, 0x10 + i * 4);
      dev->bar[i] = bar;
      if (bar != 0 && bar != 0xFFFFFFFF) {
        if (bar & 1) { // IO
          device_add_resource(pci_node, RES_IO, "bar", bar & ~0x3, 0);
        } else { // MEM
          device_add_resource(pci_node, RES_MEM, "bar", bar & ~0xF, 0);
        }
      }
    }
  }
  device_add_resource(pci_node, RES_IRQ, "irq", dev->irq_line, dev->irq_line);

  device_count++;
}

static void pci_check_device(uint8_t bus, uint8_t slot) {
  uint32_t reg0 = pci_config_read32(bus, slot, 0, 0x00);
  uint16_t vendor_id = reg0 & 0xFFFF;
  if (vendor_id == 0xFFFF)
    return;

  pci_check_function(bus, slot, 0);

  // Check if multi-function device
  uint32_t reg3 = pci_config_read32(bus, slot, 0, 0x0C);
  uint8_t header_type = (reg3 >> 16) & 0xFF;
  if (header_type & 0x80) {
    for (uint8_t func = 1; func < 8; func++) {
      pci_check_function(bus, slot, func);
    }
  }
}

// Public API

void pci_init(void) {
  pcie_init();
  device_count = 0;
  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                              " Scanning PCI bus...\n");

  for (uint16_t bus = 0; bus < 256; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      pci_check_device((uint8_t)bus, slot);
    }
  }

  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " PCI: Found ");
  print_uint32(device_count);
  console_puts(" device(s). Dumping Device Tree...\n");

  dm_dump_tree();
}

struct pci_device *pci_find_device(uint8_t class_code, uint8_t subclass) {
  for (uint32_t i = 0; i < device_count; i++) {
    if (devices[i].class_code == class_code &&
        devices[i].subclass == subclass) {
      return &devices[i];
    }
  }
  return NULL;
}

struct pci_device *pci_find_device_by_id(uint16_t vendor_id,
                                         uint16_t device_id) {
  for (uint32_t i = 0; i < device_count; i++) {
    if (devices[i].vendor_id == vendor_id &&
        devices[i].device_id == device_id) {
      return &devices[i];
    }
  }
  return NULL;
}

void pci_enable_bus_mastering(struct pci_device *dev) {
  uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
  cmd |= (1 << 2); // Set Bus Master bit
  pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id) {
  uint16_t status = pci_config_read16(dev->bus, dev->slot, dev->func, 0x06);
  if (!(status & (1 << 4)))
    return 0; // Capabilities bit not set

  uint8_t cap_ptr =
      pci_config_read32(dev->bus, dev->slot, dev->func, 0x34) & 0xFF;
  while (cap_ptr != 0) {
    uint32_t cap_reg =
        pci_config_read32(dev->bus, dev->slot, dev->func, cap_ptr);
    if ((cap_reg & 0xFF) == cap_id)
      return cap_ptr;
    cap_ptr = (cap_reg >> 8) & 0xFF;
  }
  return 0;
}

uint32_t pci_get_device_count(void) { return device_count; }

struct pci_device *pci_get_device(uint32_t index) {
  if (index >= device_count)
    return NULL;
  return &devices[index];
}
