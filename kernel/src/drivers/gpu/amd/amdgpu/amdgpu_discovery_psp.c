// SPDX-License-Identifier: GPL-2.0
/*
 * Linux v6.12 AMD discovery-table and Raphael PSP package adaptation.
 *
 * QEMU's pci-testdev has neither an AMD discovery ROM nor MP0 mailboxes.  The
 * surrogate path therefore parses a byte-exact Raphael fixture and exercises
 * the PSP ordering/rollback model without mapping a BAR or executing firmware.
 */

#include "drivers/gpu/amd/amdgpu/amdgpu_discovery_psp.h"
#include "drivers/gpu/amd/amdgpu/amdgpu.h"
#include "console/klog.h"
#include "fb/framebuffer.h"
#include "lib/string.h"

#include <linux/firmware.h>

#include <stddef.h>
#include <stdint.h>

#define AMDGPU_DISCOVERY_MAX_SIZE (10u << 10)
#define AMDGPU_DISCOVERY_BINARY_SIGNATURE 0x28211407u
#define AMDGPU_DISCOVERY_TABLE_SIGNATURE 0x53445049u
#define AMDGPU_DISCOVERY_HEADER_SIZE 60u
#define AMDGPU_DISCOVERY_TABLE_COUNT 6u
#define AMDGPU_DISCOVERY_MAX_DIES 16u
#define AMDGPU_DISCOVERY_MAX_IPS 32u
#define AMDGPU_RAPHAEL_IP_COUNT 6u
#define AMDGPU_COMMON_FW_HEADER_SIZE 32u
#define AMDGPU_TA_MAX_PACKAGES 32u
#define AMDGPU_PSP_MAILBOX_READY 0x80000000u
#define AMDGPU_PSP_GFX_CMD_LOAD_TOC 0x20u

#define AMDGPU_HWID_MP1 1u
#define AMDGPU_HWID_GC 11u
#define AMDGPU_HWID_VCN 12u
#define AMDGPU_HWID_SDMA0 42u
#define AMDGPU_HWID_MP0 255u
#define AMDGPU_HWID_DMU 271u

struct amdgpu_ip_version {
  uint16_t hw_id;
  uint8_t major;
  uint8_t minor;
  uint8_t revision;
};

struct amdgpu_discovery_result {
  struct amdgpu_ip_version ips[AMDGPU_DISCOVERY_MAX_IPS];
  uint16_t die_count;
  uint16_t ip_count;
};

enum amdgpu_psp_boot_state {
  AMDGPU_PSP_BOOT_RESET = 0,
  AMDGPU_PSP_BOOT_DISCOVERY_READY,
  AMDGPU_PSP_BOOT_FIRMWARE_READY,
  AMDGPU_PSP_BOOT_MAILBOX_READY,
  AMDGPU_PSP_BOOT_TOC_STAGED,
  AMDGPU_PSP_BOOT_COMMAND_SENT,
  AMDGPU_PSP_BOOT_COMPLETE,
  AMDGPU_PSP_BOOT_FAILED,
};

struct amdgpu_psp_boot_model {
  enum amdgpu_psp_boot_state state;
  uint32_t command_count;
  uint32_t command_id;
};

static uint16_t get_le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le16(uint8_t *p, uint16_t value) {
  p[0] = value & 0xff;
  p[1] = value >> 8;
}

static void put_le32(uint8_t *p, uint32_t value) {
  p[0] = value & 0xff;
  p[1] = (value >> 8) & 0xff;
  p[2] = (value >> 16) & 0xff;
  p[3] = value >> 24;
}

static uint16_t byte_sum(const uint8_t *data, size_t size) {
  uint16_t sum = 0;
  for (size_t i = 0; i < size; i++)
    sum = (uint16_t)(sum + data[i]);
  return sum;
}

static bool bounded_range(size_t offset, size_t size, size_t limit) {
  return offset <= limit && size <= limit - offset;
}

static bool amdgpu_discovery_parse(const uint8_t *binary, size_t available,
                                   struct amdgpu_discovery_result *result) {
  if (!binary || !result || available < AMDGPU_DISCOVERY_HEADER_SIZE ||
      available > AMDGPU_DISCOVERY_MAX_SIZE ||
      get_le32(binary) != AMDGPU_DISCOVERY_BINARY_SIGNATURE)
    return false;

  size_t binary_size = get_le16(binary + 10);
  if (binary_size < AMDGPU_DISCOVERY_HEADER_SIZE || binary_size > available ||
      byte_sum(binary + 10, binary_size - 10) != get_le16(binary + 8))
    return false;

  /* Linux table_list[IP_DISCOVERY] is the first eight-byte table_info. */
  size_t table_offset = get_le16(binary + 12);
  uint16_t table_checksum = get_le16(binary + 14);
  size_t table_size = get_le16(binary + 16);
  if (table_size < 80 || !bounded_range(table_offset, table_size, binary_size))
    return false;

  const uint8_t *table = binary + table_offset;
  if (get_le32(table) != AMDGPU_DISCOVERY_TABLE_SIGNATURE ||
      get_le16(table + 4) != 4 || get_le16(table + 6) != table_size ||
      byte_sum(table, table_size) != table_checksum)
    return false;

  uint16_t die_count = get_le16(table + 12);
  bool base_address_64 = (table[78] & 1u) != 0;
  if (!die_count || die_count > AMDGPU_DISCOVERY_MAX_DIES)
    return false;

  memset(result, 0, sizeof(*result));
  result->die_count = die_count;
  for (uint16_t die = 0; die < die_count; die++) {
    size_t die_info = 14u + (size_t)die * 4u;
    size_t die_offset = get_le16(table + die_info + 2);
    if (!bounded_range(die_offset, 4, table_size))
      return false;

    uint16_t die_id = get_le16(table + die_offset);
    uint16_t ip_count = get_le16(table + die_offset + 2);
    if (die_id != get_le16(table + die_info) ||
        ip_count > AMDGPU_DISCOVERY_MAX_IPS - result->ip_count)
      return false;

    size_t ip_offset = die_offset + 4;
    for (uint16_t ip_index = 0; ip_index < ip_count; ip_index++) {
      if (!bounded_range(ip_offset, 8, table_size))
        return false;
      const uint8_t *ip = table + ip_offset;
      size_t address_bytes = base_address_64 ? 8u : 4u;
      size_t entry_size = 8u + (size_t)ip[3] * address_bytes;
      if (!ip[3] || !bounded_range(ip_offset, entry_size, table_size))
        return false;

      struct amdgpu_ip_version *out = &result->ips[result->ip_count++];
      out->hw_id = get_le16(ip);
      out->major = ip[4];
      out->minor = ip[5];
      out->revision = ip[6];
      ip_offset += entry_size;
    }
  }
  return result->ip_count != 0;
}

static bool ip_matches(const struct amdgpu_discovery_result *result,
                       const struct amdgpu_ip_version *expected) {
  for (uint16_t i = 0; i < result->ip_count; i++) {
    const struct amdgpu_ip_version *ip = &result->ips[i];
    if (ip->hw_id == expected->hw_id && ip->major == expected->major &&
        ip->minor == expected->minor && ip->revision == expected->revision)
      return true;
  }
  return false;
}

static bool discovery_is_raphael(const struct amdgpu_discovery_result *result) {
  static const struct amdgpu_ip_version expected[] = {
      {AMDGPU_HWID_MP1, 13, 0, 5}, {AMDGPU_HWID_GC, 10, 3, 6},
      {AMDGPU_HWID_VCN, 3, 1, 2},  {AMDGPU_HWID_SDMA0, 5, 2, 6},
      {AMDGPU_HWID_MP0, 13, 0, 5}, {AMDGPU_HWID_DMU, 3, 1, 5},
  };
  if (!result || result->die_count != 1 ||
      result->ip_count != AMDGPU_RAPHAEL_IP_COUNT)
    return false;
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
    if (!ip_matches(result, &expected[i]))
      return false;
  }
  return true;
}

static size_t build_raphael_discovery_fixture(uint8_t *binary, size_t capacity) {
  static const struct amdgpu_ip_version fixture_ips[] = {
      {AMDGPU_HWID_MP1, 13, 0, 5}, {AMDGPU_HWID_GC, 10, 3, 6},
      {AMDGPU_HWID_VCN, 3, 1, 2},  {AMDGPU_HWID_SDMA0, 5, 2, 6},
      {AMDGPU_HWID_MP0, 13, 0, 5}, {AMDGPU_HWID_DMU, 3, 1, 5},
  };
  const size_t table_offset = 64;
  const size_t die_offset = 80;
  const size_t table_size = die_offset + 4 + sizeof(fixture_ips) / sizeof(fixture_ips[0]) * 12;
  const size_t binary_size = table_offset + table_size;
  if (!binary || capacity < binary_size)
    return 0;

  memset(binary, 0, capacity);
  put_le32(binary, AMDGPU_DISCOVERY_BINARY_SIGNATURE);
  put_le16(binary + 4, 1);
  put_le16(binary + 6, 0);
  put_le16(binary + 10, (uint16_t)binary_size);
  put_le16(binary + 12, (uint16_t)table_offset);
  put_le16(binary + 16, (uint16_t)table_size);

  uint8_t *table = binary + table_offset;
  put_le32(table, AMDGPU_DISCOVERY_TABLE_SIGNATURE);
  put_le16(table + 4, 4);
  put_le16(table + 6, (uint16_t)table_size);
  put_le16(table + 12, 1);
  put_le16(table + 14, 0);
  put_le16(table + 16, (uint16_t)die_offset);
  put_le16(table + die_offset, 0);
  put_le16(table + die_offset + 2,
           (uint16_t)(sizeof(fixture_ips) / sizeof(fixture_ips[0])));

  size_t offset = die_offset + 4;
  for (size_t i = 0; i < sizeof(fixture_ips) / sizeof(fixture_ips[0]); i++) {
    put_le16(table + offset, fixture_ips[i].hw_id);
    table[offset + 2] = 0;
    table[offset + 3] = 1;
    table[offset + 4] = fixture_ips[i].major;
    table[offset + 5] = fixture_ips[i].minor;
    table[offset + 6] = fixture_ips[i].revision;
    put_le32(table + offset + 8, 0x1000u + (uint32_t)i * 0x100u);
    offset += 12;
  }

  put_le16(binary + 14, byte_sum(table, table_size));
  put_le16(binary + 8, byte_sum(binary + 10, binary_size - 10));
  return binary_size;
}

static bool discovery_self_test(struct amdgpu_discovery_result *result) {
  uint8_t fixture[256];
  size_t size = build_raphael_discovery_fixture(fixture, sizeof(fixture));
  if (!size || !amdgpu_discovery_parse(fixture, size, result) ||
      !discovery_is_raphael(result))
    return false;

  /* Both enclosing and nested corruption must fail closed. */
  fixture[size - 1] ^= 1;
  put_le16(fixture + 8, byte_sum(fixture + 10, size - 10));
  if (amdgpu_discovery_parse(fixture, size, result))
    return false;
  fixture[size - 1] ^= 1;
  put_le16(fixture + 8, byte_sum(fixture + 10, size - 10));
  if (amdgpu_discovery_parse(fixture, size - 1, result))
    return false;
  return amdgpu_discovery_parse(fixture, size, result) &&
         discovery_is_raphael(result);
}

static bool common_payload_bounds(const struct firmware *fw, uint16_t major,
                                  uint16_t ip_major, uint16_t ip_minor) {
  if (!fw || !fw->data || fw->size < AMDGPU_COMMON_FW_HEADER_SIZE ||
      fw->size > UINT32_MAX)
    return false;
  const uint8_t *h = fw->data;
  uint32_t size = get_le32(h);
  uint32_t header_size = get_le32(h + 4);
  uint32_t payload_size = get_le32(h + 20);
  uint32_t payload_offset = get_le32(h + 24);
  return size == fw->size && get_le16(h + 8) == major &&
         get_le16(h + 12) == ip_major && get_le16(h + 14) == ip_minor &&
         header_size >= AMDGPU_COMMON_FW_HEADER_SIZE && header_size <= size &&
         payload_offset >= header_size &&
         bounded_range(payload_offset, payload_size, size);
}

static bool validate_ta_v2_descriptors(const struct firmware *fw) {
  if (!common_payload_bounds(fw, 2, 13, 0) || fw->size < 36)
    return false;
  const uint8_t *data = fw->data;
  uint32_t count = get_le32(data + 32);
  uint32_t payload_offset = get_le32(data + 24);
  if (!count || count >= AMDGPU_TA_MAX_PACKAGES ||
      !bounded_range(36, (size_t)count * 16u, payload_offset))
    return false;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *desc = data + 36u + (size_t)i * 16u;
    uint32_t type = get_le32(desc);
    uint32_t offset = get_le32(desc + 8);
    uint32_t size = get_le32(desc + 12);
    if (!type || !size || !bounded_range(offset, size, fw->size))
      return false;
  }
  return true;
}

static bool validate_psp_packages(struct device *device) {
  const struct firmware *toc = NULL;
  const struct firmware *ta = NULL;
  bool valid = request_firmware(&toc, "amdgpu/psp_13_0_5_toc.bin", device) == 0 &&
               common_payload_bounds(toc, 1, 13, 0) &&
               request_firmware(&ta, "amdgpu/psp_13_0_5_ta.bin", device) == 0 &&
               validate_ta_v2_descriptors(ta);
  release_firmware(ta);
  release_firmware(toc);
  return valid;
}

static bool psp_advance(struct amdgpu_psp_boot_model *model,
                        enum amdgpu_psp_boot_state next, uint32_t mailbox_status) {
  if (!model || model->state == AMDGPU_PSP_BOOT_FAILED)
    return false;
  bool ordered =
      (model->state == AMDGPU_PSP_BOOT_RESET && next == AMDGPU_PSP_BOOT_DISCOVERY_READY) ||
      (model->state == AMDGPU_PSP_BOOT_DISCOVERY_READY && next == AMDGPU_PSP_BOOT_FIRMWARE_READY) ||
      (model->state == AMDGPU_PSP_BOOT_FIRMWARE_READY && next == AMDGPU_PSP_BOOT_MAILBOX_READY) ||
      (model->state == AMDGPU_PSP_BOOT_MAILBOX_READY && next == AMDGPU_PSP_BOOT_TOC_STAGED) ||
      (model->state == AMDGPU_PSP_BOOT_TOC_STAGED && next == AMDGPU_PSP_BOOT_COMMAND_SENT) ||
      (model->state == AMDGPU_PSP_BOOT_COMMAND_SENT && next == AMDGPU_PSP_BOOT_COMPLETE);
  bool mailbox_transition = next == AMDGPU_PSP_BOOT_MAILBOX_READY ||
                            next == AMDGPU_PSP_BOOT_COMPLETE;
  if (!ordered || (mailbox_transition &&
                   mailbox_status != AMDGPU_PSP_MAILBOX_READY)) {
    model->state = AMDGPU_PSP_BOOT_FAILED;
    return false;
  }
  if (next == AMDGPU_PSP_BOOT_COMMAND_SENT) {
    model->command_count++;
    model->command_id = AMDGPU_PSP_GFX_CMD_LOAD_TOC;
  }
  model->state = next;
  return true;
}

static bool psp_model_self_test(void) {
  struct amdgpu_psp_boot_model good = {0};
  bool completed =
      psp_advance(&good, AMDGPU_PSP_BOOT_DISCOVERY_READY, 0) &&
      psp_advance(&good, AMDGPU_PSP_BOOT_FIRMWARE_READY, 0) &&
      psp_advance(&good, AMDGPU_PSP_BOOT_MAILBOX_READY,
                  AMDGPU_PSP_MAILBOX_READY) &&
      psp_advance(&good, AMDGPU_PSP_BOOT_TOC_STAGED, 0) &&
      psp_advance(&good, AMDGPU_PSP_BOOT_COMMAND_SENT, 0) &&
      psp_advance(&good, AMDGPU_PSP_BOOT_COMPLETE,
                  AMDGPU_PSP_MAILBOX_READY) &&
      good.command_count == 1 &&
      good.command_id == AMDGPU_PSP_GFX_CMD_LOAD_TOC;

  struct amdgpu_psp_boot_model failed = {0};
  bool rejected = !psp_advance(&failed, AMDGPU_PSP_BOOT_COMMAND_SENT, 0) &&
                  failed.state == AMDGPU_PSP_BOOT_FAILED &&
                  failed.command_count == 0;

  struct amdgpu_psp_boot_model status_error = {0};
  bool error_rejected =
      psp_advance(&status_error, AMDGPU_PSP_BOOT_DISCOVERY_READY, 0) &&
      psp_advance(&status_error, AMDGPU_PSP_BOOT_FIRMWARE_READY, 0) &&
      !psp_advance(&status_error, AMDGPU_PSP_BOOT_MAILBOX_READY,
                   AMDGPU_PSP_MAILBOX_READY | 1u) &&
      status_error.state == AMDGPU_PSP_BOOT_FAILED &&
      status_error.command_count == 0;
  return completed && rejected && error_rejected;
}

bool amdgpu_discovery_psp_init(struct amdgpu_device *adev) {
  if (!adev || adev->stage < AMDGPU_PORT_FIRMWARE_READY)
    return false;
  if (adev->stage >= AMDGPU_PORT_PSP_MODEL_READY)
    return true;

  void *fb_base = fb_get_base();
  uint32_t fb_width = fb_get_width();
  uint32_t fb_height = fb_get_height();
  uint32_t fb_pitch = fb_get_pitch();
  struct amdgpu_discovery_result result;
  if (!discovery_self_test(&result) || !validate_psp_packages(adev->dev) ||
      !psp_model_self_test()) {
    klog_puts("[AMDGPU] Phase 3 validation failed; PSP hardware untouched\n");
    return false;
  }
  if (fb_base != fb_get_base() || fb_width != fb_get_width() ||
      fb_height != fb_get_height() || fb_pitch != fb_get_pitch()) {
    klog_puts("[AMDGPU] Phase 3 refused: validation changed GOP state\n");
    return false;
  }

  adev->ip_discovery_validated = true;
  adev->ip_count = result.ip_count;
  adev->psp_model_validated = true;
  adev->psp_hardware_started = false;
  adev->stage = AMDGPU_PORT_PSP_MODEL_READY;
  klog_puts("[AMDGPU] Phase 3 ready: Raphael IP discovery 6/6; PSP 13.0.5 firmware descriptors and rollback model passed; hardware not executed; GOP preserved\n");
  return true;
}
