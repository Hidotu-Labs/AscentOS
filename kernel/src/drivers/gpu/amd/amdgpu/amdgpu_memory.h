/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASCENT_AMDGPU_MEMORY_H
#define ASCENT_AMDGPU_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct amdgpu_device;
struct amdgpu_bo;

bool amdgpu_phase4_memory_init(struct amdgpu_device *adev);
void amdgpu_phase4_memory_fini(void);
struct amdgpu_bo *amdgpu_bo_alloc(size_t size);
void amdgpu_bo_free(struct amdgpu_bo *bo);
bool amdgpu_bo_reserve(struct amdgpu_bo *bo);
void amdgpu_bo_unreserve(struct amdgpu_bo *bo);
bool amdgpu_bo_place_gtt(struct amdgpu_bo *bo);
bool amdgpu_bo_place_system(struct amdgpu_bo *bo);
bool amdgpu_bo_pin(struct amdgpu_bo *bo);
bool amdgpu_bo_unpin(struct amdgpu_bo *bo);
bool amdgpu_bo_map_gpuva(struct amdgpu_bo *bo, uint64_t va, uint64_t bo_offset, uint64_t size);
bool amdgpu_bo_unmap_gpuva(struct amdgpu_bo *bo, uint64_t va);
void *amdgpu_bo_cpu_address(const struct amdgpu_bo *bo);
uint64_t amdgpu_bo_physical_address(const struct amdgpu_bo *bo);
uint64_t amdgpu_bo_gtt_address(const struct amdgpu_bo *bo);
size_t amdgpu_bo_size(const struct amdgpu_bo *bo);

#endif
