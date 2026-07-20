/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_AMDGPU_H
#define ASCENT_AMDGPU_H

#include <stdbool.h>
#include <stdint.h>

struct device;
struct pci_device;

#define AMDGPU_VENDOR_ID 0x1002
#define AMDGPU_RAPHAEL_DEVICE_ID 0x164e
#define AMDGPU_QEMU_TEST_VENDOR_ID 0x1b36
#define AMDGPU_QEMU_TEST_DEVICE_ID 0x0005

enum amdgpu_port_stage {
  AMDGPU_PORT_UNBOUND = 0,
  AMDGPU_PORT_PCI_BOUND,
  AMDGPU_PORT_FIRMWARE_READY,
  AMDGPU_PORT_PSP_MODEL_READY,
  AMDGPU_PORT_MEMORY_MODEL_READY,
  AMDGPU_PORT_RING_MODEL_READY,
  AMDGPU_PORT_DISPLAY_MODEL_READY,
  AMDGPU_PORT_UAPI_MODEL_READY,
};

struct amdgpu_device {
  struct device *dev;
  struct pci_device *pdev;
  enum amdgpu_port_stage stage;
  uint16_t subsystem_vendor;
  uint16_t subsystem_device;
  uint8_t revision;
  bool qemu_surrogate;
  bool has_msi;
  bool has_msix;
  bool firmware_framebuffer_present;
  bool firmware_framebuffer_preserved;
  bool firmware_validated;
  uint32_t firmware_count;
  uint64_t firmware_bytes;
  bool ip_discovery_validated;
  uint16_t ip_count;
  bool psp_model_validated;
  bool psp_hardware_started;
  bool memory_model_validated;
  uint64_t gtt_size;
  bool gart_hardware_programmed;
  bool ring_model_validated;
  uint32_t ring_submission_count;
  bool ring_hardware_started;
  bool display_model_validated;
  uint32_t dcn_pipe_count;
  uint64_t vblank_count;
  bool display_hardware_started;
  bool uapi_model_validated;
  uint32_t uapi_context_count;
  bool render_node_published;
};

void amdgpu_init(void);
bool amdgpu_phase2_init(void);
bool amdgpu_phase3_init(void);
bool amdgpu_phase4_init(void);
bool amdgpu_phase5_init(void);
bool amdgpu_phase6_init(void);
bool amdgpu_phase7_init(void);
bool amdgpu_device_bound(void);
bool amdgpu_raphael_bound(void);
const struct amdgpu_device *amdgpu_primary_device(void);

#endif
