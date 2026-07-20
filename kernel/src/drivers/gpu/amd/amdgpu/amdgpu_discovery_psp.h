/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_AMDGPU_DISCOVERY_PSP_H
#define ASCENT_AMDGPU_DISCOVERY_PSP_H

#include <stdbool.h>

struct amdgpu_device;

bool amdgpu_discovery_psp_init(struct amdgpu_device *adev);

#endif
