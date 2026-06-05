#include "drivers/virtio/virtio_pci.h"
#include "drivers/virtio/virtio.h"
#include "drivers/pci/pci.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "console/klog.h"
#include "lib/string.h"
#include <stdint.h>
#include <stdbool.h>

// Helpers



// BAR Mapping

// Decode BAR size by writing all 1s and reading back.
static uint64_t pci_bar_size(struct pci_device *pci, int bar_idx) {
  uint8_t reg = 0x10 + bar_idx * 4;
  uint32_t orig = pci_config_read32(pci->bus, pci->slot, pci->func, reg);

  pci_config_write32(pci->bus, pci->slot, pci->func, reg, 0xFFFFFFFF);
  uint32_t sized = pci_config_read32(pci->bus, pci->slot, pci->func, reg);
  pci_config_write32(pci->bus, pci->slot, pci->func, reg, orig);

  if (orig & 1) {
    // IO BAR
    return (~(sized & 0xFFFC) + 1) & 0xFFFF;
  }

  // Memory BAR
  uint64_t mask = ~(uint64_t)0xF;
  uint64_t size64 = sized & mask;

  // 64-bit BAR?
  if (((orig >> 1) & 3) == 2) {
    uint32_t orig_hi = pci_config_read32(pci->bus, pci->slot, pci->func,
                                         reg + 4);
    pci_config_write32(pci->bus, pci->slot, pci->func, reg + 4, 0xFFFFFFFF);
    uint32_t sized_hi = pci_config_read32(pci->bus, pci->slot, pci->func,
                                          reg + 4);
    pci_config_write32(pci->bus, pci->slot, pci->func, reg + 4, orig_hi);
    size64 |= ((uint64_t)sized_hi << 32);
  }

  return (~size64 + 1);
}

// Map a PCI BAR into kernel virtual address space.
// MMIO BARs are NOT part of Limine's HHDM — we must explicitly create
// page table entries.  We place them at phys + hhdm_offset so the rest
// of the driver code can use a uniform translation.
// Returns kernel virtual address, or 0 on failure.
static uint64_t map_bar(struct pci_device *pci, int bar_idx, uint64_t *out_size) {
  uint32_t bar_lo = pci->bar[bar_idx];

  if (bar_lo & 1) {
    // IO BAR — not memory mapped, use port I/O
    klog_puts("[VIRTIO-PCI] BAR");
    klog_putchar('0' + bar_idx);
    klog_puts(" is I/O — skipping\n");
    return 0;
  }

  uint64_t phys = bar_lo & 0xFFFFFFF0;
  int type = (bar_lo >> 1) & 3;

  if (type == 2 && bar_idx < 5) {
    // 64-bit BAR
    phys |= ((uint64_t)pci->bar[bar_idx + 1] << 32);
  }

  uint64_t size = pci_bar_size(pci, bar_idx);
  if (out_size) *out_size = size;

  if (size == 0 || phys == 0) return 0;

  // Map each page of the BAR into kernel address space.
  // We place it at the HHDM address (phys + hhdm_offset) for consistency,
  // but since MMIO isn't covered by Limine's HHDM we create the entries
  // ourselves with PWT+PCD (write-through, cache-disable) for MMIO safety.
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t virt_base = phys + hhdm;
  uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t *pml4 = vmm_get_active_pml4();

  // PWT (bit 3) + PCD (bit 4) = uncacheable/write-through for MMIO
  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | (1ULL << 3) | (1ULL << 4);

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t p = phys + i * PAGE_SIZE;
    uint64_t v = virt_base + i * PAGE_SIZE;
    if (!vmm_map_page(pml4, v, p, flags)) {
      klog_puts("[VIRTIO-PCI] FATAL: vmm_map_page failed for BAR\n");
      return 0;
    }
    vmm_flush_tlb(v);
  }

  klog_puts("[VIRTIO-PCI] BAR");
  klog_putchar('0' + bar_idx);
  klog_puts(" phys=");
  klog_hex64(phys);
  klog_puts(" size=");
  klog_hex32((uint32_t)size);
  klog_puts(" mapped ");
  klog_uint64(num_pages);
  klog_puts(" pages at virt=");
  klog_hex64(virt_base);
  klog_puts("\n");

  return virt_base;
}

// PCI Capability Walking

// Read a VirtIO PCI capability at 'cap_off' in config space.
static void read_pci_cap(struct pci_device *pci, uint8_t cap_off,
                         struct virtio_pci_cap *out) {
  uint32_t dw0 = pci_config_read32(pci->bus, pci->slot, pci->func, cap_off);
  uint32_t dw1 = pci_config_read32(pci->bus, pci->slot, pci->func,
                                   cap_off + 4);
  uint32_t dw2 = pci_config_read32(pci->bus, pci->slot, pci->func,
                                   cap_off + 8);
  uint32_t dw3 = pci_config_read32(pci->bus, pci->slot, pci->func,
                                   cap_off + 12);

  out->cap_vndr  = dw0 & 0xFF;
  out->cap_next  = (dw0 >> 8) & 0xFF;
  out->cap_len   = (dw0 >> 16) & 0xFF;
  out->cfg_type  = (dw0 >> 24) & 0xFF;
  out->bar       = dw1 & 0xFF;
  out->id        = (dw1 >> 8) & 0xFF;
  out->padding[0] = (dw1 >> 16) & 0xFF;
  out->padding[1] = (dw1 >> 24) & 0xFF;
  out->offset    = dw2;
  out->length    = dw3;
}

// Public API

bool virtio_pci_init(struct virtio_pci_device *vdev, struct pci_device *pci) {
  memset(vdev, 0, sizeof(*vdev));
  vdev->pci = pci;

  // Enable bus mastering and memory space
  pci_enable_bus_mastering(pci);
  uint32_t cmd = pci_config_read32(pci->bus, pci->slot, pci->func, 0x04);
  cmd |= (1 << 1); // Memory Space Enable
  pci_config_write32(pci->bus, pci->slot, pci->func, 0x04, cmd);

  // Walk PCI capabilities
  uint32_t status = pci_config_read32(pci->bus, pci->slot, pci->func, 0x04);
  if (!((status >> 16) & (1 << 4))) {
    klog_puts("[VIRTIO-PCI] No capabilities list\n");
    return false;
  }

  uint8_t cap_off = pci_config_read32(pci->bus, pci->slot, pci->func, 0x34) &
                    0xFF;

  bool found_common = false, found_notify = false;
  bool found_isr = false;

  while (cap_off != 0 && cap_off != 0xFF) {
    uint32_t cap_header = pci_config_read32(pci->bus, pci->slot, pci->func,
                                            cap_off);
    uint8_t cap_id = cap_header & 0xFF;

    if (cap_id == 0x09) {
      // Vendor-specific = VirtIO
      struct virtio_pci_cap cap;
      read_pci_cap(pci, cap_off, &cap);

      // Map the BAR if not yet mapped
      if (cap.bar < 6 && vdev->bar_virt[cap.bar] == 0) {
        vdev->bar_virt[cap.bar] = map_bar(pci, cap.bar, &vdev->bar_size[cap.bar]);
      }

      uint64_t base = vdev->bar_virt[cap.bar];
      if (base == 0) {
        cap_off = (cap_header >> 8) & 0xFF;
        continue;
      }

      switch (cap.cfg_type) {
      case VIRTIO_PCI_CAP_COMMON_CFG:
        vdev->common = (volatile struct virtio_pci_common_cfg *)(base +
                                                                  cap.offset);
        found_common = true;
        klog_puts("[VIRTIO-PCI] Found COMMON_CFG at BAR");
        klog_putchar('0' + cap.bar);
        klog_puts("+");
        klog_hex32(cap.offset);
        klog_puts("\n");
        break;

      case VIRTIO_PCI_CAP_NOTIFY_CFG:
        vdev->notify_base = (volatile uint16_t *)(base + cap.offset);
        // The multiplier is at cap_off + 16 (after the standard 16-byte cap)
        vdev->notify_off_multiplier = pci_config_read32(
            pci->bus, pci->slot, pci->func, cap_off + 16);
        found_notify = true;
        klog_puts("[VIRTIO-PCI] Found NOTIFY_CFG at BAR");
        klog_putchar('0' + cap.bar);
        klog_puts("+");
        klog_hex32(cap.offset);
        klog_puts(" mult=");
        klog_hex32(vdev->notify_off_multiplier);
        klog_puts("\n");
        break;

      case VIRTIO_PCI_CAP_ISR_CFG:
        vdev->isr = (volatile uint8_t *)(base + cap.offset);
        found_isr = true;
        klog_puts("[VIRTIO-PCI] Found ISR_CFG\n");
        break;

      case VIRTIO_PCI_CAP_DEVICE_CFG:
        vdev->device_cfg = (volatile uint8_t *)(base + cap.offset);
        klog_puts("[VIRTIO-PCI] Found DEVICE_CFG at BAR");
        klog_putchar('0' + cap.bar);
        klog_puts("+");
        klog_hex32(cap.offset);
        klog_puts("\n");
        break;

      default:
        break;
      }
    }

    cap_off = (cap_header >> 8) & 0xFF;
  }

  if (!found_common || !found_notify || !found_isr) {
    klog_puts("[VIRTIO-PCI] Missing required capabilities (common=");
    klog_putchar(found_common ? '1' : '0');
    klog_puts(" notify=");
    klog_putchar(found_notify ? '1' : '0');
    klog_puts(" isr=");
    klog_putchar(found_isr ? '1' : '0');
    klog_puts(")\n");
    return false;
  }

  return true;
}

uint64_t virtio_pci_read_features(struct virtio_pci_device *vdev) {
  volatile struct virtio_pci_common_cfg *cfg = vdev->common;

  cfg->device_feature_select = 0;
  __asm__ volatile("" ::: "memory");
  uint32_t lo = cfg->device_feature;

  cfg->device_feature_select = 1;
  __asm__ volatile("" ::: "memory");
  uint32_t hi = cfg->device_feature;

  return ((uint64_t)hi << 32) | lo;
}

void virtio_pci_write_features(struct virtio_pci_device *vdev,
                               uint64_t features) {
  volatile struct virtio_pci_common_cfg *cfg = vdev->common;

  cfg->driver_feature_select = 0;
  __asm__ volatile("" ::: "memory");
  cfg->driver_feature = (uint32_t)(features & 0xFFFFFFFF);

  cfg->driver_feature_select = 1;
  __asm__ volatile("" ::: "memory");
  cfg->driver_feature = (uint32_t)(features >> 32);
}

void virtio_pci_set_status(struct virtio_pci_device *vdev, uint8_t status) {
  vdev->common->device_status = status;
  __asm__ volatile("" ::: "memory");
}

uint8_t virtio_pci_get_status(struct virtio_pci_device *vdev) {
  return vdev->common->device_status;
}

void virtio_pci_reset(struct virtio_pci_device *vdev) {
  vdev->common->device_status = 0;
  __asm__ volatile("" ::: "memory");
  // Wait for reset to complete (status reads back as 0)
  while (vdev->common->device_status != 0)
    __asm__ volatile("pause");
}

bool virtio_pci_setup_queue(struct virtio_pci_device *vdev,
                            uint16_t queue_index,
                            struct virtqueue *vq) {
  volatile struct virtio_pci_common_cfg *cfg = vdev->common;

  // Select the queue
  cfg->queue_select = queue_index;
  __asm__ volatile("" ::: "memory");

  // Read the maximum queue size
  uint16_t max_size = cfg->queue_size;
  if (max_size == 0) {
    klog_puts("[VIRTIO-PCI] Queue ");
    klog_uint64(queue_index);
    klog_puts(" not available (size=0)\n");
    return false;
  }

  // Cap to our maximum
  uint16_t qsz = max_size;
  if (qsz > VIRTQ_MAX_SIZE)
    qsz = VIRTQ_MAX_SIZE;

  // Initialize the virtqueue memory
  if (!virtq_init(vq, qsz)) {
    klog_puts("[VIRTIO-PCI] Failed to allocate virtqueue memory\n");
    return false;
  }

  // Write the queue size back (we may have reduced it)
  cfg->queue_size = qsz;
  __asm__ volatile("" ::: "memory");

  // Tell the device where the descriptor table, available ring,
  // and used ring are located
  cfg->queue_desc  = vq->desc_phys;
  cfg->queue_avail = vq->avail_phys;
  cfg->queue_used  = vq->used_phys;
  __asm__ volatile("" ::: "memory");

  // Compute the notify address for this queue
  uint16_t notify_off = cfg->queue_notify_off;
  vq->notify_addr = (volatile uint16_t *)((uint8_t *)vdev->notify_base +
                                           notify_off *
                                               vdev->notify_off_multiplier);

  // Disable MSI-X for this queue (use ISR polling)
  cfg->queue_msix_vector = 0xFFFF;
  __asm__ volatile("" ::: "memory");

  // Enable the queue
  cfg->queue_enable = 1;
  __asm__ volatile("" ::: "memory");

  klog_puts("[VIRTIO-PCI] Queue ");
  klog_uint64(queue_index);
  klog_puts(" initialized: size=");
  klog_uint64(qsz);
  klog_puts(" desc=");
  klog_hex64(vq->desc_phys);
  klog_puts("\n");

  return true;
}

void virtio_pci_notify(struct virtio_pci_device *vdev, uint16_t queue_index,
                       struct virtqueue *vq) {
  (void)vdev;
  (void)queue_index;
  __asm__ volatile("" ::: "memory"); // Ensure all writes are visible
  *vq->notify_addr = queue_index;
}
