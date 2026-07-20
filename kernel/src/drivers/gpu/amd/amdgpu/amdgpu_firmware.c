// SPDX-License-Identifier: GPL-2.0
/*
 * Raphael firmware discovery and validation, adapted from Linux v6.12.
 *
 * This phase only reads firmware into temporary kernel buffers. It does not
 * upload microcode, map a PCI BAR, enable bus mastering, or touch registers.
 */

#include "drivers/gpu/amd/amdgpu/amdgpu_firmware.h"
#include "drivers/gpu/amd/amdgpu/amdgpu.h"
#include "console/klog.h"
#include "fb/framebuffer.h"

#include <linux/firmware.h>

#include <stddef.h>
#include <stdint.h>

#define AMDGPU_COMMON_FW_HEADER_SIZE 32u

struct amdgpu_firmware_manifest_entry {
  const char *name;
  uint16_t ip_major;
  uint16_t ip_minor;
};

/*
 * Linux v6.12 selects these names from Raphael's discovered IP versions:
 * GC 10.3.6, SDMA 5.2.6, MP0 13.0.5, VCN 3.1.2 and DCN 3.1.5.
 * VCN's common header reports the firmware ABI as 3.0.
 */
static const struct amdgpu_firmware_manifest_entry raphael_firmware[] = {
    {"amdgpu/gc_10_3_6_ce.bin", 10, 3},
    {"amdgpu/gc_10_3_6_pfp.bin", 10, 3},
    {"amdgpu/gc_10_3_6_me.bin", 10, 3},
    {"amdgpu/gc_10_3_6_mec.bin", 10, 3},
    {"amdgpu/gc_10_3_6_mec2.bin", 10, 3},
    {"amdgpu/gc_10_3_6_rlc.bin", 10, 3},
    {"amdgpu/sdma_5_2_6.bin", 5, 2},
    {"amdgpu/psp_13_0_5_toc.bin", 13, 0},
    {"amdgpu/psp_13_0_5_ta.bin", 13, 0},
    {"amdgpu/vcn_3_1_2.bin", 3, 0},
    {"amdgpu/dcn_3_1_5_dmcub.bin", 3, 1},
};

static uint16_t get_le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool amdgpu_ucode_validate(
    const struct firmware *firmware,
    const struct amdgpu_firmware_manifest_entry *entry) {
  if (!firmware || !entry || !firmware->data ||
      firmware->size < AMDGPU_COMMON_FW_HEADER_SIZE ||
      firmware->size > UINT32_MAX)
    return false;

  const uint8_t *header = firmware->data;
  uint32_t size_bytes = get_le32(header + 0);
  uint32_t header_size = get_le32(header + 4);
  uint16_t header_major = get_le16(header + 8);
  uint16_t ip_major = get_le16(header + 12);
  uint16_t ip_minor = get_le16(header + 14);
  uint32_t ucode_size = get_le32(header + 20);
  uint32_t ucode_offset = get_le32(header + 24);
  uint32_t declared_crc = get_le32(header + 28);

  /* Linux validates exact total size; retain that rule and add safe bounds. */
  if (size_bytes != firmware->size ||
      header_size < AMDGPU_COMMON_FW_HEADER_SIZE || header_size > size_bytes ||
      !header_major || !ucode_size || !declared_crc ||
      ucode_offset < header_size || ucode_offset > size_bytes ||
      ucode_size > size_bytes - ucode_offset)
    return false;

  return ip_major == entry->ip_major && ip_minor == entry->ip_minor;
}

bool amdgpu_phase2_firmware_init(struct amdgpu_device *adev) {
  if (!adev)
    return false;
  if (adev->stage >= AMDGPU_PORT_FIRMWARE_READY)
    return true;

  void *fb_base_before = fb_get_base();
  uint32_t fb_width_before = fb_get_width();
  uint32_t fb_height_before = fb_get_height();
  uint32_t fb_pitch_before = fb_get_pitch();
  uint64_t total_bytes = 0;
  size_t validated = 0;

  for (size_t i = 0;
       i < sizeof(raphael_firmware) / sizeof(raphael_firmware[0]); i++) {
    const struct firmware *firmware = NULL;
    int error = request_firmware(&firmware, raphael_firmware[i].name, adev->dev);
    if (error || !amdgpu_ucode_validate(firmware, &raphael_firmware[i])) {
      klog_puts("[AMDGPU] Phase 2 firmware validation failed: ");
      klog_puts(raphael_firmware[i].name);
      klog_puts(error ? " (load error)\n" : " (invalid header)\n");
      release_firmware(firmware);
      return false;
    }
    total_bytes += firmware->size;
    validated++;
    release_firmware(firmware);
  }

  bool framebuffer_preserved =
      fb_base_before == fb_get_base() && fb_width_before == fb_get_width() &&
      fb_height_before == fb_get_height() && fb_pitch_before == fb_get_pitch();
  if (!framebuffer_preserved) {
    klog_puts("[AMDGPU] Phase 2 refused: firmware read changed GOP state\n");
    return false;
  }

  adev->firmware_count = (uint32_t)validated;
  adev->firmware_bytes = total_bytes;
  adev->firmware_validated = true;
  adev->stage = AMDGPU_PORT_FIRMWARE_READY;

  klog_puts("[AMDGPU] Phase 2 ready: validated ");
  klog_uint64(validated);
  klog_puts(" Raphael firmware blobs (");
  klog_uint64(total_bytes);
  klog_puts(" bytes); firmware not executed; GOP preserved\n");
  return true;
}
