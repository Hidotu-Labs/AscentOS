/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_AMDGPU_UAPI_H
#define ASCENT_AMDGPU_UAPI_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpu/amd/amdgpu/amdgpu_ring.h"

struct amdgpu_bo;
struct amdgpu_device;

#define AMDGPU_UAPI_CONTEXT_MAX 4u
#define AMDGPU_UAPI_VM_BIND_MAX 8u

struct amdgpu_uapi_context {
  bool active;
  uint32_t id;
};

bool amdgpu_phase7_uapi_init(struct amdgpu_device *adev);
void amdgpu_phase7_uapi_fini(void);
struct amdgpu_uapi_context *amdgpu_uapi_context_create(void);
void amdgpu_uapi_context_destroy(struct amdgpu_uapi_context *context);
bool amdgpu_uapi_vm_bind(struct amdgpu_uapi_context *context,
                         struct amdgpu_bo *bo, uint64_t va, uint64_t size);
bool amdgpu_uapi_vm_unbind(struct amdgpu_uapi_context *context,
                           struct amdgpu_bo *bo, uint64_t va);
bool amdgpu_uapi_submit(struct amdgpu_uapi_context *context,
                        enum amdgpu_ring_engine engine,
                        const struct amdgpu_command *command,
                        struct amdgpu_fence *fence);

#endif
