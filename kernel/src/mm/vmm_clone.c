#include "vmm.h"
#include "../lock/spinlock.h"
#include "pmm.h"
#include "tlb_shootdown.h"
#include "vma.h"
#include <stddef.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

static uint64_t *clone_table(uint64_t *src_table_phys, int level,
                              size_t start, size_t end) {
  void *new_table_phys = pmm_alloc();
  if (!new_table_phys)
    return NULL;

  uint64_t *new_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
  uint64_t *src_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_table_phys);

  for (size_t i = 0; i < 512; i++)
    new_virt[i] = 0;

  for (size_t i = start; i < end; i++) {
    if (!(src_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_virt[i] & PAGE_FLAG_USER))
      continue; // skip kernel / Limine mappings

    if (level == 1) {
      // Leaf: allocate a fresh page and copy content.
      void *new_page_phys = pmm_alloc();
      if (!new_page_phys)
        return NULL;

      uint64_t *dst64 = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_page_phys);
      uint64_t *src64 = (uint64_t *)PHYS_TO_VIRT(src_virt[i] & PAGE_MASK);
      for (size_t w = 0; w < 512; w++)
        dst64[w] = src64[w];

      new_virt[i] =
          ((uint64_t)new_page_phys & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
    } else {
      uint64_t *child_src_phys = (uint64_t *)(src_virt[i] & PAGE_MASK);
      uint64_t *child_new_phys =
          clone_table(child_src_phys, level - 1, 0, 512);
      if (!child_new_phys)
        return NULL;

      new_virt[i] =
          ((uint64_t)child_new_phys & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
    }
  }

  return (uint64_t *)new_table_phys;
}

// Return true when `vaddr` falls inside a MAP_SHARED VMA.
static bool is_shared_vma(struct vma_list *vmas, uint64_t vaddr) {
  if (!vmas)
    return false;
  struct vma *vma = vma_find(vmas, vaddr);
  return vma && (vma->flags & MAP_SHARED);
}

static uint64_t *clone_table_vma(uint64_t *src_table_phys, int level,
                                  size_t start, size_t end,
                                  struct vma_list *vmas, uint64_t base_addr) {
  void *new_table_phys = pmm_alloc();
  if (!new_table_phys)
    return NULL;

  uint64_t *new_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
  uint64_t *src_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_table_phys);

  for (size_t i = 0; i < 512; i++)
    new_virt[i] = 0;

  for (size_t i = start; i < end; i++) {
    if (!(src_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_virt[i] & PAGE_FLAG_USER))
      continue;

    if (level == 1) {
      uint64_t page_vaddr = base_addr | (i << 12);

      if (is_shared_vma(vmas, page_vaddr)) {
        // Shared: alias the physical frame.
        new_virt[i] = src_virt[i];
      } else {
        // Private: CoW — mark both sides read-only and bump the refcount.
        uint64_t phys = src_virt[i] & PAGE_MASK;
        if (pmm_is_managed(phys)) {
          if (src_virt[i] & PAGE_FLAG_RW) {
            src_virt[i] &= ~PAGE_FLAG_RW;
            src_virt[i] |= PAGE_FLAG_COW;
            // Flush the parent's stale RW TLB entry on ALL CPUs.
            // Without this, remote CPUs that cached the old RW entry
            // can still write through it, bypassing CoW.
            tlb_shootdown_page(page_vaddr);
          }
          pmm_incref((void *)phys);
        }
        new_virt[i] = src_virt[i];
      }
    } else {
      int      shift      = 12 + 9 * (level - 1);
      uint64_t child_base = base_addr | ((uint64_t)i << shift);

      uint64_t *child_src_phys = (uint64_t *)(src_virt[i] & PAGE_MASK);
      uint64_t *child_new_phys =
          clone_table_vma(child_src_phys, level - 1, 0, 512, vmas, child_base);
      if (!child_new_phys)
        return NULL;

      new_virt[i] =
          ((uint64_t)child_new_phys & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
    }
  }

  return (uint64_t *)new_table_phys;
}


uint64_t vmm_clone_user_mappings(uint64_t *src_pml4_phys) {
  spinlock_t *lock = vmm_get_lock();
  spinlock_acquire(lock);

  void *new_pml4_phys = pmm_alloc();
  if (!new_pml4_phys) {
    spinlock_release(lock);
    return 0;
  }

  uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
  uint64_t *src_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_pml4_phys);

  for (size_t i = 0; i < 512; i++)
    new_pml4_virt[i] = 0;

  // Deep-copy user half (0-255).
  for (size_t i = 0; i < 256; i++) {
    if (!(src_pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_pml4_virt[i] & PAGE_FLAG_USER))
      continue;

    uint64_t *child_src_phys = (uint64_t *)(src_pml4_virt[i] & PAGE_MASK);
    uint64_t *child_new_phys = clone_table(child_src_phys, 3, 0, 512);
    if (!child_new_phys) {
      spinlock_release(lock);
      return 0;
    }

    new_pml4_virt[i] =
        ((uint64_t)child_new_phys & PAGE_MASK) | (src_pml4_virt[i] & ~PAGE_MASK);
  }

  // Shallow-copy kernel half (256-511): shared between parent and child.
  for (size_t i = 256; i < 512; i++)
    new_pml4_virt[i] = src_pml4_virt[i];

  spinlock_release(lock);
  return (uint64_t)new_pml4_phys;
}

uint64_t vmm_clone_user_mappings_vma(uint64_t *src_pml4_phys,
                                     struct vma_list *vmas) {
  spinlock_t *lock = vmm_get_lock();
  spinlock_acquire(lock);

  void *new_pml4_phys = pmm_alloc();
  if (!new_pml4_phys) {
    spinlock_release(lock);
    return 0;
  }

  uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
  uint64_t *src_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_pml4_phys);

  for (size_t i = 0; i < 512; i++)
    new_pml4_virt[i] = 0;

  // Clone user half with VMA awareness.
  for (size_t i = 0; i < 256; i++) {
    if (!(src_pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_pml4_virt[i] & PAGE_FLAG_USER))
      continue;

    uint64_t base_addr = (uint64_t)i << 39;
    // Sign-extend if bit 47 is set (canonical address).
    if (base_addr & (1ULL << 47))
      base_addr |= 0xFFFF000000000000ULL;

    uint64_t *child_src_phys = (uint64_t *)(src_pml4_virt[i] & PAGE_MASK);
    uint64_t *child_new_phys =
        clone_table_vma(child_src_phys, 3, 0, 512, vmas, base_addr);
    if (!child_new_phys) {
      spinlock_release(lock);
      return 0;
    }

    new_pml4_virt[i] =
        ((uint64_t)child_new_phys & PAGE_MASK) | (src_pml4_virt[i] & ~PAGE_MASK);
  }

  // Shallow-copy kernel half.
  for (size_t i = 256; i < 512; i++)
    new_pml4_virt[i] = src_pml4_virt[i];

  spinlock_release(lock);
  return (uint64_t)new_pml4_phys;
}

uint64_t *vmm_create_pml4(void) {
  spinlock_t *lock = vmm_get_lock();
  spinlock_acquire(lock);

  void *new_pml4_phys = pmm_alloc();
  if (!new_pml4_phys) {
    spinlock_release(lock);
    return NULL;
  }

  uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
  uint64_t *src_pml4_virt =
      (uint64_t *)PHYS_TO_VIRT((uint64_t)vmm_get_kernel_pml4());

  // Blank user half.
  for (size_t i = 0; i < 256; i++)
    new_pml4_virt[i] = 0;

  // Shared kernel half.
  for (size_t i = 256; i < 512; i++)
    new_pml4_virt[i] = src_pml4_virt[i];

  spinlock_release(lock);
  return (uint64_t *)new_pml4_phys;
}
