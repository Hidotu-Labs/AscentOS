// SPDX-License-Identifier: GPL-2.0
/* Bounded pre-publication AMDGPU render ABI model; no DRM ioctls are exposed. */

#include "drivers/gpu/amd/amdgpu/amdgpu_uapi.h"
#include "drivers/gpu/amd/amdgpu/amdgpu.h"
#include "drivers/gpu/amd/amdgpu/amdgpu_memory.h"
#include "console/klog.h"
#include "drivers/gpu/drm/drm.h"
#include "fb/framebuffer.h"
#include "lib/string.h"

#include <stdint.h>

struct amdgpu_uapi_vm_bind {
  bool active;
  struct amdgpu_uapi_context *context;
  struct amdgpu_bo *bo;
  uint64_t va;
};

static struct amdgpu_uapi_context contexts[AMDGPU_UAPI_CONTEXT_MAX];
static struct amdgpu_uapi_vm_bind bindings[AMDGPU_UAPI_VM_BIND_MAX];
static uint32_t next_context_id;
static bool uapi_ready;

static bool context_known(const struct amdgpu_uapi_context *context) {
  return context && context >= contexts &&
         context < contexts + AMDGPU_UAPI_CONTEXT_MAX && context->active;
}

static bool binding_known(const struct amdgpu_uapi_context *context,
                          const struct amdgpu_bo *bo) {
  for (uint32_t i = 0; i < AMDGPU_UAPI_VM_BIND_MAX; i++)
    if (bindings[i].active && bindings[i].context == context &&
        bindings[i].bo == bo)
      return true;
  return false;
}

struct amdgpu_uapi_context *amdgpu_uapi_context_create(void) {
  if (!uapi_ready)
    return NULL;
  for (uint32_t i = 0; i < AMDGPU_UAPI_CONTEXT_MAX; i++) {
    if (!contexts[i].active) {
      contexts[i].active = true;
      contexts[i].id = ++next_context_id;
      if (!contexts[i].id)
        contexts[i].id = ++next_context_id;
      return &contexts[i];
    }
  }
  return NULL;
}

void amdgpu_uapi_context_destroy(struct amdgpu_uapi_context *context) {
  if (!context_known(context))
    return;
  for (uint32_t i = 0; i < AMDGPU_UAPI_VM_BIND_MAX; i++) {
    if (bindings[i].active && bindings[i].context == context) {
      amdgpu_bo_unmap_gpuva(bindings[i].bo, bindings[i].va);
      memset(&bindings[i], 0, sizeof(bindings[i]));
    }
  }
  memset(context, 0, sizeof(*context));
}

bool amdgpu_uapi_vm_bind(struct amdgpu_uapi_context *context,
                         struct amdgpu_bo *bo, uint64_t va, uint64_t size) {
  if (!context_known(context) || !bo || binding_known(context, bo))
    return false;
  for (uint32_t i = 0; i < AMDGPU_UAPI_VM_BIND_MAX; i++) {
    if (!bindings[i].active && amdgpu_bo_map_gpuva(bo, va, 0, size)) {
      bindings[i] = (struct amdgpu_uapi_vm_bind){.active = true,
          .context = context, .bo = bo, .va = va};
      return true;
    }
  }
  return false;
}

bool amdgpu_uapi_vm_unbind(struct amdgpu_uapi_context *context,
                           struct amdgpu_bo *bo, uint64_t va) {
  if (!context_known(context))
    return false;
  for (uint32_t i = 0; i < AMDGPU_UAPI_VM_BIND_MAX; i++) {
    if (bindings[i].active && bindings[i].context == context &&
        bindings[i].bo == bo && bindings[i].va == va &&
        amdgpu_bo_unmap_gpuva(bo, va)) {
      memset(&bindings[i], 0, sizeof(bindings[i]));
      return true;
    }
  }
  return false;
}

bool amdgpu_uapi_submit(struct amdgpu_uapi_context *context,
                        enum amdgpu_ring_engine engine,
                        const struct amdgpu_command *command,
                        struct amdgpu_fence *fence) {
  if (!context_known(context) || !command || !binding_known(context, command->dst) ||
      (command->src && !binding_known(context, command->src)))
    return false;
  return amdgpu_submit_kernel_command(engine, command, fence);
}

static bool prepare_bo(struct amdgpu_bo *bo) {
  return bo && amdgpu_bo_reserve(bo) && amdgpu_bo_place_gtt(bo) &&
         amdgpu_bo_pin(bo);
}

static void release_bo(struct amdgpu_bo *bo) {
  if (!bo)
    return;
  amdgpu_bo_unpin(bo);
  amdgpu_bo_unreserve(bo);
  amdgpu_bo_free(bo);
}

static bool uapi_self_test(void) {
  struct amdgpu_bo *src = amdgpu_bo_alloc(64), *dst = amdgpu_bo_alloc(64);
  struct amdgpu_uapi_context *a = amdgpu_uapi_context_create();
  struct amdgpu_uapi_context *b = amdgpu_uapi_context_create();
  struct amdgpu_fence fence = {0};
  bool ok = src && dst && a && b && prepare_bo(src) && prepare_bo(dst);
  if (!ok)
    goto out;
  *(uint32_t *)amdgpu_bo_cpu_address(src) = 0xa55a5aa5;
  struct amdgpu_command copy = {.opcode = AMDGPU_CMD_SDMA_COPY, .src = src,
      .dst = dst, .size = sizeof(uint32_t)};
  ok = amdgpu_uapi_vm_bind(a, src, 0x1000000000ull, 4096) &&
       amdgpu_uapi_vm_bind(a, dst, 0x1000001000ull, 4096) &&
       !amdgpu_uapi_submit(b, AMDGPU_RING_SDMA, &copy, &fence) &&
       amdgpu_uapi_submit(a, AMDGPU_RING_SDMA, &copy, &fence) &&
       fence.signaled && !fence.error &&
       *(uint32_t *)amdgpu_bo_cpu_address(dst) == 0xa55a5aa5 &&
       !amdgpu_uapi_vm_unbind(a, src, 0x1000002000ull);
  ok = ok && amdgpu_uapi_vm_unbind(a, src, 0x1000000000ull) &&
       amdgpu_uapi_vm_unbind(a, dst, 0x1000001000ull);
out:
  if (a) amdgpu_uapi_context_destroy(a);
  if (b) amdgpu_uapi_context_destroy(b);
  release_bo(src);
  release_bo(dst);
  return ok;
}

void amdgpu_phase7_uapi_fini(void) {
  for (uint32_t i = 0; i < AMDGPU_UAPI_CONTEXT_MAX; i++)
    amdgpu_uapi_context_destroy(&contexts[i]);
  memset(contexts, 0, sizeof(contexts));
  memset(bindings, 0, sizeof(bindings));
  next_context_id = 0;
  uapi_ready = false;
}

bool amdgpu_phase7_uapi_init(struct amdgpu_device *adev) {
  if (!adev || adev->stage < AMDGPU_PORT_DISPLAY_MODEL_READY)
    return false;
  if (adev->stage >= AMDGPU_PORT_UAPI_MODEL_READY)
    return true;
  void *fb_base = fb_get_base();
  amdgpu_phase7_uapi_fini();
  uapi_ready = true;
  if (!global_drm_dev.name || !uapi_self_test()) {
    amdgpu_phase7_uapi_fini();
    klog_puts("[AMDGPU] Phase 7 ABI validation failed; render node unpublished\n");
    return false;
  }
  if (fb_base != fb_get_base()) {
    amdgpu_phase7_uapi_fini();
    klog_puts("[AMDGPU] Phase 7 refused: validation changed GOP state\n");
    return false;
  }
  adev->uapi_model_validated = true;
  adev->uapi_context_count = 0;
  adev->render_node_published = false;
  adev->stage = AMDGPU_PORT_UAPI_MODEL_READY;
  klog_puts("[AMDGPU] Phase 7 ready: render ABI context, BO-list, GPUVM and submit validation passed; render node unpublished; Mesa disabled; GOP preserved\n");
  return true;
}
