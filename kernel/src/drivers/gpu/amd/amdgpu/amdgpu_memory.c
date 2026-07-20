// SPDX-License-Identifier: GPL-2.0
/*
 * Bounded AMDGPU buffer placement and GPUVM model, adapted around Linux's
 * TTM/AMDGPU ownership rules.  This phase creates real RAM backing but does
 * not program a GART, MMHUB page table, BAR, or GPU register.
 */

#include "drivers/gpu/amd/amdgpu/amdgpu_memory.h"
#include "drivers/gpu/amd/amdgpu/amdgpu.h"
#include "console/klog.h"
#include "fb/framebuffer.h"
#include "lib/string.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

#define AMDGPU_GTT_BASE 0x0000000100000000ull
#define AMDGPU_GTT_SIZE (256ull << 20)
#define AMDGPU_GPUVA_BASE 0x0000001000000000ull
#define AMDGPU_GPUVA_SIZE (1ull << 40)
#define AMDGPU_BO_SIZE_MAX (32ull << 20)
#define AMDGPU_BO_MAX 32u
#define AMDGPU_VM_MAPPING_MAX 64u
#define AMDGPU_GPU_ADDR_INVALID UINT64_MAX

enum amdgpu_memory_domain {
  AMDGPU_DOMAIN_SYSTEM = 0,
  AMDGPU_DOMAIN_GTT,
};

struct amdgpu_bo {
  bool active;
  bool reserved;
  enum amdgpu_memory_domain domain;
  uint32_t handle;
  uint32_t pin_count;
  uint64_t size;
  size_t pages;
  uint64_t phys;
  void *cpu;
  uint64_t gtt_addr;
};

struct amdgpu_vm_mapping {
  bool active;
  struct amdgpu_bo *bo;
  uint64_t va;
  uint64_t size;
  uint64_t bo_offset;
};

struct amdgpu_memory_manager {
  uint64_t gtt_start;
  uint64_t gtt_size;
  uint64_t va_start;
  uint64_t va_size;
  uint32_t next_handle;
  uint32_t bo_count;
  uint32_t mapping_count;
  struct amdgpu_bo bos[AMDGPU_BO_MAX];
  struct amdgpu_vm_mapping mappings[AMDGPU_VM_MAPPING_MAX];
};

static struct amdgpu_memory_manager phase4_test_mm;
static struct amdgpu_memory_manager primary_mm;
static bool primary_mm_ready;

static bool add_u64(uint64_t left, uint64_t right, uint64_t *result) {
  if (left > UINT64_MAX - right)
    return false;
  *result = left + right;
  return true;
}

static bool page_aligned(uint64_t value) {
  return (value & (PAGE_SIZE - 1u)) == 0;
}

static bool range_inside(uint64_t start, uint64_t size, uint64_t base,
                         uint64_t extent) {
  uint64_t end;
  uint64_t limit;
  return size && add_u64(start, size, &end) && add_u64(base, extent, &limit) &&
         start >= base && end <= limit;
}

static bool ranges_overlap(uint64_t a_start, uint64_t a_size,
                           uint64_t b_start, uint64_t b_size) {
  uint64_t a_end;
  uint64_t b_end;
  if (!add_u64(a_start, a_size, &a_end) ||
      !add_u64(b_start, b_size, &b_end))
    return true;
  return a_start < b_end && b_start < a_end;
}

static void amdgpu_mm_init(struct amdgpu_memory_manager *mm,
                           uint64_t gtt_start, uint64_t gtt_size) {
  memset(mm, 0, sizeof(*mm));
  mm->gtt_start = gtt_start;
  mm->gtt_size = gtt_size;
  mm->va_start = AMDGPU_GPUVA_BASE;
  mm->va_size = AMDGPU_GPUVA_SIZE;
  mm->next_handle = 1;
}

static struct amdgpu_bo *amdgpu_bo_create_in(struct amdgpu_memory_manager *mm,
                                          uint64_t requested_size) {
  if (!mm || !requested_size || requested_size > AMDGPU_BO_SIZE_MAX ||
      requested_size > UINT64_MAX - (PAGE_SIZE - 1u))
    return NULL;
  uint64_t size = (requested_size + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
  size_t pages = (size_t)(size / PAGE_SIZE);

  struct amdgpu_bo *bo = NULL;
  for (size_t i = 0; i < AMDGPU_BO_MAX; i++) {
    if (!mm->bos[i].active) {
      bo = &mm->bos[i];
      break;
    }
  }
  if (!bo)
    return NULL;

  void *phys = pmm_alloc_pages(pages);
  if (!phys)
    return NULL;
  memset(bo, 0, sizeof(*bo));
  bo->active = true;
  bo->domain = AMDGPU_DOMAIN_SYSTEM;
  bo->handle = mm->next_handle++;
  if (!bo->handle)
    bo->handle = mm->next_handle++;
  bo->size = size;
  bo->pages = pages;
  bo->phys = (uint64_t)phys;
  bo->cpu = (void *)(bo->phys + pmm_get_hhdm_offset());
  bo->gtt_addr = AMDGPU_GPU_ADDR_INVALID;
  memset(bo->cpu, 0, size);
  mm->bo_count++;
  return bo;
}

bool amdgpu_bo_reserve(struct amdgpu_bo *bo) {
  if (!bo || !bo->active || bo->reserved)
    return false;
  bo->reserved = true;
  return true;
}

void amdgpu_bo_unreserve(struct amdgpu_bo *bo) {
  if (bo && bo->active)
    bo->reserved = false;
}

static bool amdgpu_gtt_first_fit(struct amdgpu_memory_manager *mm,
                                 struct amdgpu_bo *target,
                                 uint64_t *address) {
  uint64_t candidate = mm->gtt_start;
  while (range_inside(candidate, target->size, mm->gtt_start, mm->gtt_size)) {
    bool bumped = false;
    for (size_t i = 0; i < AMDGPU_BO_MAX; i++) {
      struct amdgpu_bo *bo = &mm->bos[i];
      if (!bo->active || bo == target || bo->domain != AMDGPU_DOMAIN_GTT)
        continue;
      if (ranges_overlap(candidate, target->size, bo->gtt_addr, bo->size)) {
        if (!add_u64(bo->gtt_addr, bo->size, &candidate))
          return false;
        bumped = true;
        break;
      }
    }
    if (!bumped) {
      *address = candidate;
      return true;
    }
  }
  return false;
}

static bool amdgpu_bo_validate_in(struct amdgpu_memory_manager *mm,
                               struct amdgpu_bo *bo,
                               enum amdgpu_memory_domain domain) {
  if (!mm || !bo || !bo->active || !bo->reserved)
    return false;
  if (domain == bo->domain)
    return true;
  if (domain == AMDGPU_DOMAIN_SYSTEM) {
    if (bo->pin_count)
      return false;
    bo->domain = AMDGPU_DOMAIN_SYSTEM;
    bo->gtt_addr = AMDGPU_GPU_ADDR_INVALID;
    return true;
  }
  if (domain != AMDGPU_DOMAIN_GTT)
    return false;

  uint64_t address;
  if (!amdgpu_gtt_first_fit(mm, bo, &address))
    return false;
  bo->gtt_addr = address;
  bo->domain = AMDGPU_DOMAIN_GTT;
  return true;
}

bool amdgpu_bo_pin(struct amdgpu_bo *bo) {
  if (!bo || !bo->active || !bo->reserved ||
      bo->domain != AMDGPU_DOMAIN_GTT || bo->pin_count == UINT32_MAX)
    return false;
  bo->pin_count++;
  return true;
}

bool amdgpu_bo_unpin(struct amdgpu_bo *bo) {
  if (!bo || !bo->active || !bo->reserved || !bo->pin_count)
    return false;
  bo->pin_count--;
  return true;
}

static bool amdgpu_vm_map_in(struct amdgpu_memory_manager *mm,
                          struct amdgpu_bo *bo, uint64_t va,
                          uint64_t bo_offset, uint64_t size) {
  if (!mm || !bo || !bo->active || !bo->reserved ||
      bo->domain != AMDGPU_DOMAIN_GTT || !page_aligned(va) ||
      !page_aligned(bo_offset) || !page_aligned(size) ||
      !range_inside(va, size, mm->va_start, mm->va_size) ||
      !range_inside(bo_offset, size, 0, bo->size))
    return false;

  for (size_t i = 0; i < AMDGPU_VM_MAPPING_MAX; i++) {
    struct amdgpu_vm_mapping *mapping = &mm->mappings[i];
    if (mapping->active && ranges_overlap(va, size, mapping->va, mapping->size))
      return false;
  }
  for (size_t i = 0; i < AMDGPU_VM_MAPPING_MAX; i++) {
    struct amdgpu_vm_mapping *mapping = &mm->mappings[i];
    if (!mapping->active) {
      mapping->active = true;
      mapping->bo = bo;
      mapping->va = va;
      mapping->size = size;
      mapping->bo_offset = bo_offset;
      mm->mapping_count++;
      return true;
    }
  }
  return false;
}

static bool amdgpu_vm_unmap_in(struct amdgpu_memory_manager *mm,
                            struct amdgpu_bo *bo, uint64_t va) {
  if (!mm || !bo || !bo->active || !bo->reserved)
    return false;
  for (size_t i = 0; i < AMDGPU_VM_MAPPING_MAX; i++) {
    struct amdgpu_vm_mapping *mapping = &mm->mappings[i];
    if (mapping->active && mapping->bo == bo && mapping->va == va) {
      memset(mapping, 0, sizeof(*mapping));
      mm->mapping_count--;
      return true;
    }
  }
  return false;
}

static void amdgpu_bo_destroy_in(struct amdgpu_memory_manager *mm,
                              struct amdgpu_bo *bo) {
  if (!mm || !bo || !bo->active)
    return;
  for (size_t i = 0; i < AMDGPU_VM_MAPPING_MAX; i++) {
    if (mm->mappings[i].active && mm->mappings[i].bo == bo) {
      memset(&mm->mappings[i], 0, sizeof(mm->mappings[i]));
      mm->mapping_count--;
    }
  }
  pmm_free_pages((void *)bo->phys, bo->pages);
  memset(bo, 0, sizeof(*bo));
  mm->bo_count--;
}

static void amdgpu_mm_fini(struct amdgpu_memory_manager *mm) {
  if (!mm)
    return;
  for (size_t i = 0; i < AMDGPU_BO_MAX; i++)
    amdgpu_bo_destroy_in(mm, &mm->bos[i]);
  memset(mm, 0, sizeof(*mm));
}

static bool amdgpu_memory_self_test(void) {
  size_t free_before = pmm_get_free_pages();
  struct amdgpu_memory_manager *mm = &phase4_test_mm;
  amdgpu_mm_init(mm, AMDGPU_GTT_BASE, AMDGPU_GTT_SIZE);

  struct amdgpu_bo *a = amdgpu_bo_create_in(mm, PAGE_SIZE + 17u);
  struct amdgpu_bo *b = amdgpu_bo_create_in(mm, PAGE_SIZE * 3u);
  if (!a || !b || a->size != PAGE_SIZE * 2u ||
      !amdgpu_bo_reserve(a) || amdgpu_bo_reserve(a) ||
      !amdgpu_bo_validate_in(mm, a, AMDGPU_DOMAIN_GTT) ||
      !amdgpu_bo_reserve(b) ||
      !amdgpu_bo_validate_in(mm, b, AMDGPU_DOMAIN_GTT) ||
      a->gtt_addr != AMDGPU_GTT_BASE ||
      b->gtt_addr != AMDGPU_GTT_BASE + a->size)
    goto fail;

  if (!amdgpu_vm_map_in(mm, a, AMDGPU_GPUVA_BASE, 0, a->size) ||
      amdgpu_vm_map_in(mm, b, AMDGPU_GPUVA_BASE + PAGE_SIZE, 0, PAGE_SIZE) ||
      !amdgpu_vm_map_in(mm, b, AMDGPU_GPUVA_BASE + a->size, 0, b->size) ||
      !amdgpu_bo_pin(a) ||
      amdgpu_bo_validate_in(mm, a, AMDGPU_DOMAIN_SYSTEM) ||
      !amdgpu_bo_unpin(a) ||
      !amdgpu_vm_unmap_in(mm, a, AMDGPU_GPUVA_BASE))
    goto fail;

  uint64_t freed_gtt = a->gtt_addr;
  amdgpu_bo_unreserve(a);
  amdgpu_bo_destroy_in(mm, a);
  struct amdgpu_bo *reuse = amdgpu_bo_create_in(mm, PAGE_SIZE * 2u);
  if (!reuse || !amdgpu_bo_reserve(reuse) ||
      !amdgpu_bo_validate_in(mm, reuse, AMDGPU_DOMAIN_GTT) ||
      reuse->gtt_addr != freed_gtt)
    goto fail;

  amdgpu_mm_fini(mm);
  if (pmm_get_free_pages() != free_before)
    return false;

  /* Placement failure must leave the object valid in SYSTEM with no address. */
  amdgpu_mm_init(mm, AMDGPU_GTT_BASE, PAGE_SIZE * 4u);
  struct amdgpu_bo *oversized = amdgpu_bo_create_in(mm, PAGE_SIZE * 5u);
  if (!oversized || !amdgpu_bo_reserve(oversized) ||
      amdgpu_bo_validate_in(mm, oversized, AMDGPU_DOMAIN_GTT) ||
      oversized->domain != AMDGPU_DOMAIN_SYSTEM ||
      oversized->gtt_addr != AMDGPU_GPU_ADDR_INVALID)
    goto fail;
  amdgpu_mm_fini(mm);
  return pmm_get_free_pages() == free_before && !mm->bo_count &&
         !mm->mapping_count;

fail:
  amdgpu_mm_fini(mm);
  return false;
}

struct amdgpu_bo *amdgpu_bo_alloc(size_t size) {
  return primary_mm_ready ? amdgpu_bo_create_in(&primary_mm, size) : NULL;
}

void amdgpu_bo_free(struct amdgpu_bo *bo) {
  if (primary_mm_ready)
    amdgpu_bo_destroy_in(&primary_mm, bo);
}

bool amdgpu_bo_place_gtt(struct amdgpu_bo *bo) {
  return primary_mm_ready &&
         amdgpu_bo_validate_in(&primary_mm, bo, AMDGPU_DOMAIN_GTT);
}

bool amdgpu_bo_place_system(struct amdgpu_bo *bo) {
  return primary_mm_ready &&
         amdgpu_bo_validate_in(&primary_mm, bo, AMDGPU_DOMAIN_SYSTEM);
}

bool amdgpu_bo_map_gpuva(struct amdgpu_bo *bo, uint64_t va,
                          uint64_t bo_offset, uint64_t size) {
  return primary_mm_ready &&
         amdgpu_vm_map_in(&primary_mm, bo, va, bo_offset, size);
}

bool amdgpu_bo_unmap_gpuva(struct amdgpu_bo *bo, uint64_t va) {
  return primary_mm_ready && amdgpu_vm_unmap_in(&primary_mm, bo, va);
}

void *amdgpu_bo_cpu_address(const struct amdgpu_bo *bo) {
  return bo && bo->active ? bo->cpu : NULL;
}

uint64_t amdgpu_bo_physical_address(const struct amdgpu_bo *bo) {
  return bo && bo->active ? bo->phys : 0;
}

uint64_t amdgpu_bo_gtt_address(const struct amdgpu_bo *bo) {
  return bo && bo->active && bo->domain == AMDGPU_DOMAIN_GTT
             ? bo->gtt_addr
             : AMDGPU_GPU_ADDR_INVALID;
}

size_t amdgpu_bo_size(const struct amdgpu_bo *bo) {
  return bo && bo->active ? (size_t)bo->size : 0;
}

static bool amdgpu_memory_api_smoke_test(void) {
  size_t free_before = pmm_get_free_pages();
  struct amdgpu_bo *bo = amdgpu_bo_alloc(PAGE_SIZE);
  bool passed = bo && amdgpu_bo_reserve(bo) &&
                amdgpu_bo_place_gtt(bo) &&
                amdgpu_bo_gtt_address(bo) == AMDGPU_GTT_BASE &&
                amdgpu_bo_map_gpuva(bo, AMDGPU_GPUVA_BASE, 0, PAGE_SIZE) &&
                amdgpu_bo_unmap_gpuva(bo, AMDGPU_GPUVA_BASE) &&
                amdgpu_bo_place_system(bo);
  if (bo) {
    amdgpu_bo_unreserve(bo);
    amdgpu_bo_free(bo);
  }
  return passed && pmm_get_free_pages() == free_before;
}

void amdgpu_phase4_memory_fini(void) {
  if (!primary_mm_ready)
    return;
  amdgpu_mm_fini(&primary_mm);
  primary_mm_ready = false;
}

bool amdgpu_phase4_memory_init(struct amdgpu_device *adev) {
  if (!adev || adev->stage < AMDGPU_PORT_PSP_MODEL_READY)
    return false;
  if (adev->stage >= AMDGPU_PORT_MEMORY_MODEL_READY)
    return true;

  void *fb_base = fb_get_base();
  uint32_t fb_width = fb_get_width();
  uint32_t fb_height = fb_get_height();
  uint32_t fb_pitch = fb_get_pitch();
  if (!amdgpu_memory_self_test()) {
    klog_puts("[AMDGPU] Phase 4 memory validation failed; GART hardware untouched\n");
    return false;
  }
  if (fb_base != fb_get_base() || fb_width != fb_get_width() ||
      fb_height != fb_get_height() || fb_pitch != fb_get_pitch()) {
    klog_puts("[AMDGPU] Phase 4 refused: memory validation changed GOP state\n");
    return false;
  }

  amdgpu_mm_init(&primary_mm, AMDGPU_GTT_BASE, AMDGPU_GTT_SIZE);
  primary_mm_ready = true;
  if (!amdgpu_memory_api_smoke_test()) {
    amdgpu_phase4_memory_fini();
    klog_puts("[AMDGPU] Phase 4 public memory API validation failed; GART hardware untouched\n");
    return false;
  }
  adev->memory_model_validated = true;
  adev->gtt_size = AMDGPU_GTT_SIZE;
  adev->gart_hardware_programmed = false;
  adev->stage = AMDGPU_PORT_MEMORY_MODEL_READY;
  klog_puts("[AMDGPU] Phase 4 ready: GTT placement, GPUVM map/unmap, reservation and teardown tests passed; GART hardware not programmed; GOP preserved\n");
  return true;
}
