/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_AMDGPU_RING_H
#define ASCENT_AMDGPU_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct amdgpu_bo;
struct amdgpu_device;

enum amdgpu_ring_engine {
  AMDGPU_RING_SDMA = 0,
  AMDGPU_RING_GFX,
};

/* Kernel-internal submission shape. The Phase 7 UAPI must not expose this. */
enum amdgpu_command_opcode {
  AMDGPU_CMD_SDMA_COPY = 1,
  AMDGPU_CMD_GFX_WRITE_DW,
};

struct amdgpu_command {
  enum amdgpu_command_opcode opcode;
  struct amdgpu_bo *src;
  struct amdgpu_bo *dst;
  uint64_t src_offset;
  uint64_t dst_offset;
  uint64_t size;
  uint32_t value;
};

struct amdgpu_fence {
  uint64_t sequence;
  bool signaled;
  bool error;
};

bool amdgpu_phase5_ring_init(struct amdgpu_device *adev);
void amdgpu_phase5_ring_fini(void);
bool amdgpu_submit_kernel_command(enum amdgpu_ring_engine engine,
                                  const struct amdgpu_command *command,
                                  struct amdgpu_fence *fence);

#endif
