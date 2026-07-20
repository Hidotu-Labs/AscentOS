// SPDX-License-Identifier: GPL-2.0
/*
 * DCN/DMUB and display-interrupt ownership model.  The model establishes the
 * DRM-facing validation contract but has no AMD BAR, register or IRQ access.
 */

#include "drivers/gpu/amd/amdgpu/amdgpu_display.h"
#include "drivers/gpu/amd/amdgpu/amdgpu.h"
#include "console/klog.h"
#include "drivers/gpu/drm/drm.h"
#include "fb/framebuffer.h"
#include "lib/string.h"

#include <stdint.h>

#define AMDGPU_DCN_PIPE_MAX 1u
#define AMDGPU_DMUB_SEQUENCE_MAX 4u

enum amdgpu_dmub_state {
  AMDGPU_DMUB_OFF = 0,
  AMDGPU_DMUB_READY,
  AMDGPU_DMUB_MODE_VALIDATED,
  AMDGPU_DMUB_VBLANK_ENABLED,
};

struct amdgpu_dcn_mode {
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t refresh_hz;
};

struct amdgpu_display_model {
  struct amdgpu_dcn_mode mode;
  enum amdgpu_dmub_state dmub_state;
  uint32_t dmub_messages;
  uint64_t vblank_count;
  uint32_t hotplug_count;
  bool irq_acknowledged;
  bool attached_to_drm;
};

static struct amdgpu_display_model display_model;

static bool dcn_mode_valid(const struct amdgpu_dcn_mode *mode) {
  if (!mode || !mode->width || !mode->height || !mode->refresh_hz ||
      mode->width > UINT32_MAX / 4u)
    return false;
  return mode->pitch >= mode->width * 4u &&
         mode->pitch <= (1u << 20) && mode->height <= 16384u;
}

static bool dmub_advance(struct amdgpu_display_model *model,
                         enum amdgpu_dmub_state state) {
  if (!model || state != model->dmub_state + 1 ||
      model->dmub_messages == AMDGPU_DMUB_SEQUENCE_MAX)
    return false;
  model->dmub_state = state;
  model->dmub_messages++;
  return true;
}

bool amdgpu_display_model_irq(enum amdgpu_display_irq irq) {
  if (!display_model.attached_to_drm ||
      display_model.dmub_state != AMDGPU_DMUB_VBLANK_ENABLED)
    return false;
  display_model.irq_acknowledged = false;
  if (irq == AMDGPU_DISPLAY_IRQ_VBLANK)
    display_model.vblank_count++;
  else if (irq == AMDGPU_DISPLAY_IRQ_HOTPLUG)
    display_model.hotplug_count++;
  else
    return false;
  display_model.irq_acknowledged = true;
  return true;
}

uint64_t amdgpu_display_model_vblank_count(void) {
  return display_model.vblank_count;
}

static bool display_self_test(const struct amdgpu_dcn_mode *gop_mode) {
  struct amdgpu_display_model test = {0};
  struct amdgpu_dcn_mode invalid = *gop_mode;
  invalid.pitch = invalid.width ? invalid.width * 4u - 1u : 0;
  if (!dcn_mode_valid(gop_mode) || dcn_mode_valid(&invalid) ||
      !dmub_advance(&test, AMDGPU_DMUB_READY) ||
      !dmub_advance(&test, AMDGPU_DMUB_MODE_VALIDATED) ||
      !dmub_advance(&test, AMDGPU_DMUB_VBLANK_ENABLED) ||
      dmub_advance(&test, AMDGPU_DMUB_VBLANK_ENABLED))
    return false;
  test.attached_to_drm = true;
  display_model = test;
  if (!amdgpu_display_model_irq(AMDGPU_DISPLAY_IRQ_VBLANK) ||
      !amdgpu_display_model_irq(AMDGPU_DISPLAY_IRQ_HOTPLUG) ||
      !display_model.irq_acknowledged || display_model.vblank_count != 1 ||
      display_model.hotplug_count != 1)
    return false;
  memset(&display_model, 0, sizeof(display_model));
  return true;
}

void amdgpu_phase6_display_fini(void) {
  memset(&display_model, 0, sizeof(display_model));
}

bool amdgpu_phase6_display_init(struct amdgpu_device *adev) {
  if (!adev || adev->stage < AMDGPU_PORT_RING_MODEL_READY)
    return false;
  if (adev->stage >= AMDGPU_PORT_DISPLAY_MODEL_READY)
    return true;
  struct amdgpu_dcn_mode gop_mode = {
      .width = fb_get_width(), .height = fb_get_height(),
      .pitch = fb_get_pitch(), .refresh_hz = 60};
  void *fb_base = fb_get_base();
  if (!fb_base || !display_self_test(&gop_mode)) {
    klog_puts("[AMDGPU] Phase 6 display validation failed; DCN hardware untouched\n");
    return false;
  }
  if (fb_base != fb_get_base() || gop_mode.width != fb_get_width() ||
      gop_mode.height != fb_get_height() || gop_mode.pitch != fb_get_pitch()) {
    klog_puts("[AMDGPU] Phase 6 refused: validation changed GOP state\n");
    return false;
  }

  display_model.mode = gop_mode;
  display_model.attached_to_drm = true;
  if (!dmub_advance(&display_model, AMDGPU_DMUB_READY) ||
      !dmub_advance(&display_model, AMDGPU_DMUB_MODE_VALIDATED) ||
      !dmub_advance(&display_model, AMDGPU_DMUB_VBLANK_ENABLED) ||
      !amdgpu_display_model_irq(AMDGPU_DISPLAY_IRQ_VBLANK)) {
    amdgpu_phase6_display_fini();
    klog_puts("[AMDGPU] Phase 6 display startup failed; DCN hardware untouched\n");
    return false;
  }
  drm_ensure_outputs(&global_drm_dev, AMDGPU_DCN_PIPE_MAX);
  adev->display_model_validated = true;
  adev->dcn_pipe_count = AMDGPU_DCN_PIPE_MAX;
  adev->vblank_count = display_model.vblank_count;
  adev->display_hardware_started = false;
  adev->stage = AMDGPU_PORT_DISPLAY_MODEL_READY;
  klog_puts("[AMDGPU] Phase 6 ready: DCN/DMUB mode, vblank acknowledgement and DRM/KMS attachment passed; display hardware untouched; GOP preserved\n");
  return true;
}
