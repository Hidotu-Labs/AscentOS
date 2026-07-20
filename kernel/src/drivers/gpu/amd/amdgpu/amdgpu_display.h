/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_AMDGPU_DISPLAY_H
#define ASCENT_AMDGPU_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

struct amdgpu_device;

enum amdgpu_display_irq {
  AMDGPU_DISPLAY_IRQ_VBLANK = 0,
  AMDGPU_DISPLAY_IRQ_HOTPLUG,
};

bool amdgpu_phase6_display_init(struct amdgpu_device *adev);
void amdgpu_phase6_display_fini(void);
bool amdgpu_display_model_irq(enum amdgpu_display_irq irq);
uint64_t amdgpu_display_model_vblank_count(void);

#endif
