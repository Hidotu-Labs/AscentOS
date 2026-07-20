// SPDX-License-Identifier: GPL-2.0
/* Bounded SDMA/GFX scheduling model; hardware transport remains disabled. */

#include "drivers/gpu/amd/amdgpu/amdgpu_ring.h"
#include "drivers/gpu/amd/amdgpu/amdgpu.h"
#include "drivers/gpu/amd/amdgpu/amdgpu_memory.h"
#include "console/klog.h"
#include "fb/framebuffer.h"
#include "lib/string.h"

#include <stdint.h>

#define AMDGPU_RING_QUEUE_MAX 8u
#define AMDGPU_RING_COMMAND_MAX 16u

struct amdgpu_ring_job {
  struct amdgpu_command command;
  struct amdgpu_fence *fence;
};

struct amdgpu_ring_model {
  struct amdgpu_ring_job queue[AMDGPU_RING_QUEUE_MAX];
  uint32_t head;
  uint32_t tail;
  uint32_t queued;
  uint64_t next_sequence;
  uint64_t completed_sequence;
  uint32_t submitted;
  uint32_t recovered;
  bool faulted;
};

static struct amdgpu_ring_model rings[2];
static bool rings_ready;

static bool range_valid(const struct amdgpu_bo *bo, uint64_t offset,
                        uint64_t size) {
  size_t bo_size = amdgpu_bo_size(bo);
  return bo && size && offset <= UINT64_MAX - size && offset + size <= bo_size;
}

static bool command_valid(enum amdgpu_ring_engine engine,
                          const struct amdgpu_command *command) {
  if (!command || engine > AMDGPU_RING_GFX)
    return false;
  if (engine == AMDGPU_RING_SDMA)
    return command->opcode == AMDGPU_CMD_SDMA_COPY && command->src &&
           command->dst && command->size <= AMDGPU_RING_COMMAND_MAX * sizeof(uint32_t) &&
           range_valid(command->src, command->src_offset, command->size) &&
           range_valid(command->dst, command->dst_offset, command->size) &&
           amdgpu_bo_gtt_address(command->src) != UINT64_MAX &&
           amdgpu_bo_gtt_address(command->dst) != UINT64_MAX;
  return command->opcode == AMDGPU_CMD_GFX_WRITE_DW && command->dst &&
         !command->src && command->size == sizeof(uint32_t) &&
         !(command->dst_offset & (sizeof(uint32_t) - 1u)) &&
         range_valid(command->dst, command->dst_offset, sizeof(uint32_t)) &&
         amdgpu_bo_gtt_address(command->dst) != UINT64_MAX;
}

static bool execute_command(enum amdgpu_ring_engine engine,
                            const struct amdgpu_command *command) {
  uint8_t *dst = amdgpu_bo_cpu_address(command->dst);
  if (!dst)
    return false;
  if (engine == AMDGPU_RING_SDMA) {
    const uint8_t *src = amdgpu_bo_cpu_address(command->src);
    if (!src)
      return false;
    memcpy(dst + command->dst_offset, src + command->src_offset, command->size);
    return true;
  }
  *(uint32_t *)(dst + command->dst_offset) = command->value;
  return true;
}

static void ring_dispatch(enum amdgpu_ring_engine engine) {
  struct amdgpu_ring_model *ring = &rings[engine];
  while (!ring->faulted && ring->queued) {
    struct amdgpu_ring_job *job = &ring->queue[ring->head];
    bool completed = execute_command(engine, &job->command);
    job->fence->error = !completed;
    job->fence->signaled = true;
    ring->completed_sequence = job->fence->sequence;
    ring->head = (ring->head + 1u) % AMDGPU_RING_QUEUE_MAX;
    ring->queued--;
    if (!completed)
      ring->faulted = true;
  }
}

static bool ring_submit(enum amdgpu_ring_engine engine,
                        const struct amdgpu_command *command,
                        struct amdgpu_fence *fence) {
  if (!rings_ready || !fence || !command_valid(engine, command))
    return false;
  struct amdgpu_ring_model *ring = &rings[engine];
  if (ring->faulted || ring->queued == AMDGPU_RING_QUEUE_MAX)
    return false;
  struct amdgpu_ring_job *job = &ring->queue[ring->tail];
  *job = (struct amdgpu_ring_job){.command = *command, .fence = fence};
  fence->sequence = ++ring->next_sequence;
  fence->signaled = false;
  fence->error = false;
  ring->tail = (ring->tail + 1u) % AMDGPU_RING_QUEUE_MAX;
  ring->queued++;
  ring->submitted++;
  ring_dispatch(engine);
  return fence->signaled && !fence->error;
}

static void ring_recover(enum amdgpu_ring_engine engine) {
  struct amdgpu_ring_model *ring = &rings[engine];
  while (ring->queued) {
    struct amdgpu_ring_job *job = &ring->queue[ring->head];
    job->fence->error = true;
    job->fence->signaled = true;
    ring->completed_sequence = job->fence->sequence;
    ring->head = (ring->head + 1u) % AMDGPU_RING_QUEUE_MAX;
    ring->queued--;
  }
  ring->faulted = false;
  ring->recovered++;
}

static bool place_for_execution(struct amdgpu_bo *bo) {
  return bo && amdgpu_bo_reserve(bo) && amdgpu_bo_place_gtt(bo) && amdgpu_bo_pin(bo);
}

static void release_execution_bo(struct amdgpu_bo *bo) {
  if (!bo)
    return;
  amdgpu_bo_unpin(bo);
  amdgpu_bo_unreserve(bo);
  amdgpu_bo_free(bo);
}

static bool ring_self_test(void) {
  struct amdgpu_bo *src = amdgpu_bo_alloc(64);
  struct amdgpu_bo *dst = amdgpu_bo_alloc(64);
  struct amdgpu_fence sdma_fence = {0}, gfx_fence = {0};
  bool ok = src && dst && place_for_execution(src) && place_for_execution(dst);
  if (!ok)
    goto out;
  uint32_t *source = amdgpu_bo_cpu_address(src);
  uint32_t *target = amdgpu_bo_cpu_address(dst);
  source[0] = 0xdecafbad;
  struct amdgpu_command copy = {.opcode = AMDGPU_CMD_SDMA_COPY, .src = src,
      .dst = dst, .size = sizeof(uint32_t)};
  struct amdgpu_command write = {.opcode = AMDGPU_CMD_GFX_WRITE_DW, .dst = dst,
      .dst_offset = sizeof(uint32_t), .size = sizeof(uint32_t), .value = 0x12345678};
  struct amdgpu_command invalid = {.opcode = AMDGPU_CMD_SDMA_COPY, .src = src,
      .dst = dst, .size = 0};
  ok = ring_submit(AMDGPU_RING_SDMA, &copy, &sdma_fence) &&
       ring_submit(AMDGPU_RING_GFX, &write, &gfx_fence) &&
       sdma_fence.sequence == 1 && gfx_fence.sequence == 1 &&
       sdma_fence.signaled && gfx_fence.signaled && target[0] == source[0] &&
       target[1] == 0x12345678 && !ring_submit(AMDGPU_RING_SDMA, &invalid, &sdma_fence);
  rings[AMDGPU_RING_SDMA].faulted = true;
  ok = ok && !ring_submit(AMDGPU_RING_SDMA, &copy, &sdma_fence);
  ring_recover(AMDGPU_RING_SDMA);
  ok = ok && ring_submit(AMDGPU_RING_SDMA, &copy, &sdma_fence) &&
       sdma_fence.sequence == 2 && rings[AMDGPU_RING_SDMA].recovered == 1;
out:
  release_execution_bo(src);
  release_execution_bo(dst);
  return ok;
}

bool amdgpu_submit_kernel_command(enum amdgpu_ring_engine engine,
                                  const struct amdgpu_command *command,
                                  struct amdgpu_fence *fence) {
  return ring_submit(engine, command, fence);
}

void amdgpu_phase5_ring_fini(void) {
  memset(rings, 0, sizeof(rings));
  rings_ready = false;
}

bool amdgpu_phase5_ring_init(struct amdgpu_device *adev) {
  if (!adev || adev->stage < AMDGPU_PORT_MEMORY_MODEL_READY)
    return false;
  if (adev->stage >= AMDGPU_PORT_RING_MODEL_READY)
    return true;
  void *fb_base = fb_get_base();
  uint32_t fb_width = fb_get_width(), fb_height = fb_get_height();
  uint32_t fb_pitch = fb_get_pitch();
  amdgpu_phase5_ring_fini();
  rings_ready = true;
  if (!ring_self_test()) {
    amdgpu_phase5_ring_fini();
    klog_puts("[AMDGPU] Phase 5 ring validation failed; hardware rings untouched\n");
    return false;
  }
  if (fb_base != fb_get_base() || fb_width != fb_get_width() ||
      fb_height != fb_get_height() || fb_pitch != fb_get_pitch()) {
    amdgpu_phase5_ring_fini();
    klog_puts("[AMDGPU] Phase 5 refused: validation changed GOP state\n");
    return false;
  }
  adev->ring_model_validated = true;
  adev->ring_submission_count = rings[AMDGPU_RING_SDMA].submitted +
                                rings[AMDGPU_RING_GFX].submitted;
  adev->ring_hardware_started = false;
  adev->stage = AMDGPU_PORT_RING_MODEL_READY;
  klog_puts("[AMDGPU] Phase 5 ready: SDMA/GFX queue, fences, validation, software command execution and recovery passed; hardware rings untouched; GOP preserved\n");
  return true;
}
