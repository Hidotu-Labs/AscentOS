// SPDX-License-Identifier: GPL-2.0
/*
 * AscentOS kernel adaptation entry point for Linux DRM/AMDGPU.
 *
 * Phase 1 is deliberately config-space read-only. It establishes the Linux
 * driver's PCI ownership/lifetime shape for Raphael without enabling memory
 * decoding, bus mastering, interrupts, MMIO, firmware, or display takeover.
 * The Limine/UEFI framebuffer must remain the active fallback.
 */

#include "drivers/gpu/amd/amdgpu/amdgpu.h"
#include "drivers/gpu/amd/amdgpu/amdgpu_firmware.h"
#include "drivers/gpu/amd/amdgpu/amdgpu_discovery_psp.h"
#include "drivers/gpu/amd/amdgpu/amdgpu_memory.h"
#include "drivers/gpu/amd/amdgpu/amdgpu_ring.h"
#include "drivers/gpu/amd/amdgpu/amdgpu_display.h"
#include "drivers/gpu/amd/amdgpu/amdgpu_uapi.h"
#include "console/klog.h"
#include "drivers/manager/device.h"
#include "drivers/pci/pci.h"
#include "fb/framebuffer.h"
#include "lib/string.h"

#include <stddef.h>
#include <stdint.h>

static struct amdgpu_device primary_adev;

static struct pci_device *amdgpu_pci_from_device(struct device *dev) {
  if (!dev)
    return NULL;
  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *pdev = pci_get_device(i);
    if (pdev && pdev->kernel_device == dev)
      return pdev;
  }
  return NULL;
}

static void amdgpu_log_bdf(const struct pci_device *pdev) {
  if (!pdev)
    return;
  char bdf[16];
  snprintf(bdf, sizeof(bdf), "0000:%02x:%02x.%x", pdev->bus, pdev->slot,
           pdev->func);
  klog_puts(bdf);
}

static void amdgpu_log_resources(const struct device *dev) {
  if (!dev)
    return;
  for (size_t i = 0; i < dev->resource_count; i++) {
    const struct resource *resource = &dev->resources[i];
    if (resource->type != RES_MEM && resource->type != RES_IO)
      continue;
    klog_puts("[AMDGPU] ");
    klog_puts(resource->name);
    klog_puts(resource->type == RES_MEM ? " MMIO " : " I/O ");
    klog_hex64(resource->start);
    klog_puts("-");
    klog_hex64(resource->end);
    klog_puts(" size=");
    klog_uint64(resource->end - resource->start + 1);
    klog_puts("\n");
  }
}

static int amdgpu_pci_probe(struct device *dev) {
  struct pci_device *pdev = amdgpu_pci_from_device(dev);
  if (!pdev)
    return -1;
  bool raphael = pdev->vendor_id == AMDGPU_VENDOR_ID &&
                  pdev->device_id == AMDGPU_RAPHAEL_DEVICE_ID;
  bool qemu_surrogate = pdev->vendor_id == AMDGPU_QEMU_TEST_VENDOR_ID &&
                        pdev->device_id == AMDGPU_QEMU_TEST_DEVICE_ID;
  if ((!raphael && !qemu_surrogate) || (raphael && pdev->class_code != 0x03))
    return -1;

  void *fb_base_before = fb_get_base();
  uint32_t fb_width_before = fb_get_width();
  uint32_t fb_height_before = fb_get_height();
  uint32_t fb_pitch_before = fb_get_pitch();
  uint16_t command_before =
      pci_config_read16(pdev->bus, pdev->slot, pdev->func, 0x04);
  uint32_t revision_class =
      pci_config_read32(pdev->bus, pdev->slot, pdev->func, 0x08);
  uint32_t subsystem =
      pci_config_read32(pdev->bus, pdev->slot, pdev->func, 0x2c);

  memset(&primary_adev, 0, sizeof(primary_adev));
  primary_adev.dev = dev;
  primary_adev.pdev = pdev;
  primary_adev.revision = revision_class & 0xff;
  primary_adev.qemu_surrogate = qemu_surrogate;
  primary_adev.subsystem_vendor = subsystem & 0xffff;
  primary_adev.subsystem_device = subsystem >> 16;
  primary_adev.has_msi =
      pci_find_capability(pdev, PCI_CAP_ID_MSI) != 0;
  primary_adev.has_msix =
      pci_find_capability(pdev, PCI_CAP_ID_MSIX) != 0;
  primary_adev.firmware_framebuffer_present =
      fb_base_before && fb_width_before && fb_height_before && fb_pitch_before;

  uint16_t command_after =
      pci_config_read16(pdev->bus, pdev->slot, pdev->func, 0x04);
  primary_adev.firmware_framebuffer_preserved =
      command_before == command_after &&
      fb_base_before == fb_get_base() &&
      fb_width_before == fb_get_width() &&
      fb_height_before == fb_get_height() &&
      fb_pitch_before == fb_get_pitch();
  if (!primary_adev.firmware_framebuffer_preserved) {
    memset(&primary_adev, 0, sizeof(primary_adev));
    klog_puts("[AMDGPU] refusing PCI surrogate/Raphael bind: GOP/config invariant changed\n");
    return -1;
  }

  primary_adev.stage = AMDGPU_PORT_PCI_BOUND;
  dev->driver_data = &primary_adev;

  klog_puts(qemu_surrogate
                ? "[AMDGPU] Linux port Phase 1 QEMU surrogate 1b36:0005 at "
                : "[AMDGPU] Linux port Phase 1 detected Raphael 1002:164e at ");
  amdgpu_log_bdf(pdev);
  klog_puts(" revision=");
  klog_hex32(primary_adev.revision);
  klog_puts(" subsystem=");
  klog_hex32(((uint32_t)primary_adev.subsystem_device << 16) |
             primary_adev.subsystem_vendor);
  klog_puts("\n");
  amdgpu_log_resources(dev);
  klog_puts("[AMDGPU] interrupt capabilities: MSI=");
  klog_puts(primary_adev.has_msi ? "yes" : "no");
  klog_puts(" MSI-X=");
  klog_puts(primary_adev.has_msix ? "yes" : "no");
  klog_puts("\n");
  if (primary_adev.firmware_framebuffer_present)
    klog_puts(qemu_surrogate
                  ? "[AMDGPU] Phase 1 ready: QEMU surrogate bound read-only; GOP preserved\n"
                  : "[AMDGPU] Phase 1 ready: PCI bound read-only; GOP preserved\n");
  else
    klog_puts(qemu_surrogate
                  ? "[AMDGPU] Phase 1 ready: QEMU surrogate bound read-only; no GOP present\n"
                  : "[AMDGPU] Phase 1 ready: PCI bound read-only; no GOP present\n");
  return 0;
}

static void amdgpu_pci_remove(struct device *dev) {
  amdgpu_phase7_uapi_fini();
  amdgpu_phase6_display_fini();
  amdgpu_phase5_ring_fini();
  amdgpu_phase4_memory_fini();
  if (dev && dev->driver_data == &primary_adev)
    dev->driver_data = NULL;
  memset(&primary_adev, 0, sizeof(primary_adev));
}

/*
 * Raphael's integrated display function is 1002:164e. Later phases will
 * import Linux's broader PCI/IP-discovery matching only after the common
 * initialization and rollback machinery exists.
 */
static struct device_id amdgpu_pciidlist[] = {
    {.type = ID_PCI,
     .pci = {.vendor = AMDGPU_VENDOR_ID,
             .device = AMDGPU_RAPHAEL_DEVICE_ID,
             .match_class = false}},
    {.type = ID_PCI,
     .pci = {.vendor = AMDGPU_QEMU_TEST_VENDOR_ID,
             .device = AMDGPU_QEMU_TEST_DEVICE_ID,
             .match_class = false}},
};

static struct driver amdgpu_kms_pci_driver = {
    .name = "amdgpu",
    .ids = amdgpu_pciidlist,
    .id_count = sizeof(amdgpu_pciidlist) / sizeof(amdgpu_pciidlist[0]),
    .probe = amdgpu_pci_probe,
    .remove = amdgpu_pci_remove,
    .kind = DRIVER_KERNEL,
};

void amdgpu_init(void) {
  memset(&primary_adev, 0, sizeof(primary_adev));
  amdgpu_kms_pci_driver.bus = pci_bus_type();
  dm_register_driver(&amdgpu_kms_pci_driver);
}

bool amdgpu_device_bound(void) {
  return primary_adev.stage >= AMDGPU_PORT_PCI_BOUND &&
         primary_adev.dev && primary_adev.pdev;
}

bool amdgpu_phase2_init(void) {
  if (!amdgpu_device_bound())
    return false;
  return amdgpu_phase2_firmware_init(&primary_adev);
}

bool amdgpu_phase3_init(void) {
  if (!amdgpu_device_bound())
    return false;
  return amdgpu_discovery_psp_init(&primary_adev);
}

bool amdgpu_phase4_init(void) {
  if (!amdgpu_device_bound())
    return false;
  return amdgpu_phase4_memory_init(&primary_adev);
}

bool amdgpu_phase5_init(void) {
  if (!amdgpu_device_bound())
    return false;
  return amdgpu_phase5_ring_init(&primary_adev);
}

bool amdgpu_phase6_init(void) {
  if (!amdgpu_device_bound())
    return false;
  return amdgpu_phase6_display_init(&primary_adev);
}

bool amdgpu_phase7_init(void) {
  if (!amdgpu_device_bound())
    return false;
  return amdgpu_phase7_uapi_init(&primary_adev);
}

bool amdgpu_raphael_bound(void) {
  return amdgpu_device_bound() && !primary_adev.qemu_surrogate;
}

const struct amdgpu_device *amdgpu_primary_device(void) {
  return amdgpu_device_bound() ? &primary_adev : NULL;
}
