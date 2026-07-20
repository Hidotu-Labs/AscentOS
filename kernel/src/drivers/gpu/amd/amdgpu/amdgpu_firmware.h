/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_AMDGPU_FIRMWARE_H
#define ASCENT_AMDGPU_FIRMWARE_H

#include <stdbool.h>

struct amdgpu_device;

bool amdgpu_phase2_firmware_init(struct amdgpu_device *adev);

#endif
