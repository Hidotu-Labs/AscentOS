#include "vmm.h"
#include "../console/klog.h"
#include "../lock/spinlock.h"
#include "pmm.h"
#include <stddef.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

static spinlock_t vmm_lock = SPINLOCK_INIT;

spinlock_t *vmm_get_lock(void) { return &vmm_lock; }

static uint64_t *get_next_level(uint64_t *current_level, size_t index,
                                bool allocate) {
  if (current_level[index] & PAGE_FLAG_PRESENT) {
    uint64_t next_phys = current_level[index] & PAGE_MASK;
    return (uint64_t *)PHYS_TO_VIRT(next_phys);
  }

  if (!allocate)
    return NULL;

  void *new_table_phys = pmm_alloc();
  if (!new_table_phys)
    return NULL;

  uint64_t *new_table_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
  for (size_t i = 0; i < 512; i++)
    new_table_virt[i] = 0;

  current_level[index] = ((uint64_t)new_table_phys) | PAGE_FLAG_PRESENT |
                         PAGE_FLAG_RW | PAGE_FLAG_USER;
  return new_table_virt;
}

bool vmm_map_page(uint64_t *pml4, uint64_t virtual_addr, uint64_t physical_addr,
                  uint64_t flags) {
  spinlock_acquire(&vmm_lock);
  bool success = false;

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt      = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);
  uint64_t  propagate_flags = flags & (PAGE_FLAG_USER | PAGE_FLAG_RW);

  uint64_t *pdpt_virt = get_next_level(pml4_virt, pml4_index, true);
  if (!pdpt_virt) {
    klog_puts("[VMM] Error: Failed to get/create PDPT for vaddr 0x");
    klog_uint64(virtual_addr);
    klog_puts("\n");
    goto unlock;
  }
  pml4_virt[pml4_index] |= propagate_flags;

  uint64_t *pd_virt = get_next_level(pdpt_virt, pdpt_index, true);
  if (!pd_virt) {
    klog_puts("[VMM] Error: Failed to get/create PD for vaddr 0x");
    klog_uint64(virtual_addr);
    klog_puts("\n");
    goto unlock;
  }
  pdpt_virt[pdpt_index] |= propagate_flags;

  uint64_t *pt_virt = get_next_level(pd_virt, pd_index, true);
  if (!pt_virt) {
    klog_puts("[VMM] Error: Failed to get/create PT for vaddr 0x");
    klog_uint64(virtual_addr);
    klog_puts("\n");
    goto unlock;
  }
  pd_virt[pd_index] |= propagate_flags;

  pt_virt[pt_index] = (physical_addr & PAGE_MASK) | flags | PAGE_FLAG_PRESENT;
  vmm_flush_tlb(virtual_addr);
  success = true;

unlock:
  spinlock_release(&vmm_lock);
  return success;
}

bool vmm_map_huge_page(uint64_t *pml4, uint64_t virtual_addr,
                       uint64_t physical_addr, uint64_t flags) {
  spinlock_acquire(&vmm_lock);
  bool success = false;

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;

  uint64_t *pml4_virt      = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);
  uint64_t  propagate_flags = flags & (PAGE_FLAG_USER | PAGE_FLAG_RW);

  uint64_t *pdpt_virt = get_next_level(pml4_virt, pml4_index, true);
  if (!pdpt_virt)
    goto unlock;
  pml4_virt[pml4_index] |= propagate_flags;

  uint64_t *pd_virt = get_next_level(pdpt_virt, pdpt_index, true);
  if (!pd_virt)
    goto unlock;
  pdpt_virt[pdpt_index] |= propagate_flags;

  pd_virt[pd_index] =
      (physical_addr & PAGE_MASK) | flags | PAGE_FLAG_PRESENT | PAGE_FLAG_PS;
  vmm_flush_tlb(virtual_addr);
  success = true;

unlock:
  spinlock_release(&vmm_lock);
  return success;
}

bool vmm_map_range(uint64_t *pml4, uint64_t virtual_addr,
                   uint64_t physical_addr, size_t pages, uint64_t flags) {
  for (size_t i = 0; i < pages; i++) {
    if (!vmm_map_page(pml4, virtual_addr + (i * 4096),
                      physical_addr + (i * 4096), flags)) {
      return false;
    }
  }
  return true;
}

void vmm_free_empty_tables(uint64_t *pml4, uint64_t virtual_addr) {
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);
  if (!(pml4_virt[pml4_index] & PAGE_FLAG_PRESENT))
    return;

  uint64_t  pdpt_phys = pml4_virt[pml4_index] & PAGE_MASK;
  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(pdpt_phys);
  if (!(pdpt_virt[pdpt_index] & PAGE_FLAG_PRESENT) ||
      (pdpt_virt[pdpt_index] & PAGE_FLAG_PS))
    return;

  uint64_t  pd_phys = pdpt_virt[pdpt_index] & PAGE_MASK;
  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pd_phys);
  if (!(pd_virt[pd_index] & PAGE_FLAG_PRESENT) ||
      (pd_virt[pd_index] & PAGE_FLAG_PS))
    return;

  uint64_t  pt_phys = pd_virt[pd_index] & PAGE_MASK;
  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pt_phys);

  // Check if PT is empty.
  bool pt_empty = true;
  for (int i = 0; i < 512; i++) {
    if (pt_virt[i] & PAGE_FLAG_PRESENT) {
      pt_empty = false;
      break;
    }
  }

  if (pt_empty) {
    pd_virt[pd_index] = 0;
    vmm_flush_tlb(virtual_addr);
    pmm_free_page((void *)pt_phys);

    bool pd_empty = true;
    for (int i = 0; i < 512; i++) {
      if (pd_virt[i] & PAGE_FLAG_PRESENT) {
        pd_empty = false;
        break;
      }
    }

    if (pd_empty) {
      pdpt_virt[pdpt_index] = 0;
      vmm_flush_tlb(virtual_addr);
      pmm_free_page((void *)pd_phys);

      bool pdpt_empty = true;
      for (int i = 0; i < 512; i++) {
        if (pdpt_virt[i] & PAGE_FLAG_PRESENT) {
          pdpt_empty = false;
          break;
        }
      }

      if (pdpt_empty) {
        pml4_virt[pml4_index] = 0;
        vmm_flush_tlb(virtual_addr);
        pmm_free_page((void *)pdpt_phys);
      }
    }
  }
}

void vmm_unmap_page(uint64_t *pml4, uint64_t virtual_addr) {
  spinlock_acquire(&vmm_lock);

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);

  if (!(pml4_virt[pml4_index] & PAGE_FLAG_PRESENT))
    goto unlock;
  uint64_t *pdpt_virt =
      (uint64_t *)PHYS_TO_VIRT(pml4_virt[pml4_index] & PAGE_MASK);

  uint64_t pdpt_entry = pdpt_virt[pdpt_index];
  if (!(pdpt_entry & PAGE_FLAG_PRESENT))
    goto unlock;
  if (pdpt_entry & PAGE_FLAG_PS)
    goto unlock; // 1GB page — not supported at 4KB granularity

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pdpt_entry & PAGE_MASK);
  uint64_t  pd_entry = pd_virt[pd_index];
  if (!(pd_entry & PAGE_FLAG_PRESENT))
    goto unlock;
  if (pd_entry & PAGE_FLAG_PS)
    goto unlock; // 2MB page

  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pd_entry & PAGE_MASK);
  pt_virt[pt_index] = 0;
  vmm_flush_tlb(virtual_addr);

  if (pml4_index < 256 ||
      (pml4_index >= 256 && virtual_addr < KERNEL_HEAP_BASE)) {
    vmm_free_empty_tables(pml4, virtual_addr);
  }

unlock:
  spinlock_release(&vmm_lock);
}

uint64_t vmm_virt_to_phys(uint64_t *pml4_phys, uint64_t virtual_addr) {
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys);
  uint64_t  entry;

  entry = pml4_virt[pml4_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;

  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pdpt_virt[pdpt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  if (entry & PAGE_FLAG_PS) // 1GB huge page
    return (entry & 0xFFFFFC0000000ULL) | (virtual_addr & 0x3FFFFFFFULL);

  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pd_virt[pd_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  if (entry & PAGE_FLAG_PS) // 2MB huge page
    return (entry & 0xFFFFFFFE00000ULL) | (virtual_addr & 0x1FFFFFULL);

  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pt_virt[pt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  return (entry & PAGE_MASK) | (virtual_addr & 0xFFFULL);
}
