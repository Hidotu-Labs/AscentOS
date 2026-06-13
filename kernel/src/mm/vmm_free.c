#include "vmm.h"
#include "../console/klog.h"
#include "pmm.h"
#include "vma.h"
#include <stddef.h>
#include <stdint.h>

void vmm_free_user_pages(uint64_t cr3) {
  if (cr3 == 0)
    return;

  // Safety: never free the active PML4.
  uint64_t active_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
  if (cr3 == (active_cr3 & PAGE_MASK)) {
    klog_puts("[VMM] WARNING: refusing to free active CR3!\n");
    return;
  }

  uint64_t  hhdm     = pmm_get_hhdm_offset();
  uint64_t *pml4_virt = (uint64_t *)(hhdm + cr3);

  for (size_t i = 0; i < 256; i++) {
    if (!(pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;

    uint64_t  pdpt_phys = pml4_virt[i] & PAGE_MASK;
    uint64_t *pdpt_virt = (uint64_t *)(hhdm + pdpt_phys);

    for (size_t j = 0; j < 512; j++) {
      if (!(pdpt_virt[j] & PAGE_FLAG_PRESENT))
        continue;
      if (pdpt_virt[j] & PAGE_FLAG_PS) // 1GB huge page — skip
        continue;

      uint64_t  pd_phys = pdpt_virt[j] & PAGE_MASK;
      uint64_t *pd_virt = (uint64_t *)(hhdm + pd_phys);

      for (size_t k = 0; k < 512; k++) {
        if (!(pd_virt[k] & PAGE_FLAG_PRESENT))
          continue;

        if (pd_virt[k] & PAGE_FLAG_PS) {
          // 2MB huge page: free all 512 constituent 4KB frames.
          uint64_t huge_phys = pd_virt[k] & PAGE_MASK;
          for (size_t p = 0; p < 512; p++)
            pmm_free_page((void *)(huge_phys + p * 4096));
          pd_virt[k] = 0;
          continue;
        }

        uint64_t  pt_phys = pd_virt[k] & PAGE_MASK;
        uint64_t *pt_virt = (uint64_t *)(hhdm + pt_phys);

        for (size_t l = 0; l < 512; l++) {
          if (pt_virt[l] & PAGE_FLAG_PRESENT)
            pmm_free_page((void *)(pt_virt[l] & PAGE_MASK));
        }
        pmm_free_page((void *)pt_phys);
      }
      pmm_free_page((void *)pd_phys);
    }
    pmm_free_page((void *)pdpt_phys);
  }

  pmm_free_page((void *)cr3);
}

void vmm_free_user_pages_vma(uint64_t cr3, struct vma_list *vmas) {
  if (cr3 == 0)
    return;

  if (!vmas) {
    vmm_free_user_pages(cr3);
    return;
  }

  uint64_t active_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
  if (cr3 == (active_cr3 & PAGE_MASK)) {
    klog_puts("[VMM] WARNING: refusing to free active CR3!\n");
    return;
  }

  uint64_t  hhdm     = pmm_get_hhdm_offset();
  uint64_t *pml4_virt = (uint64_t *)(hhdm + cr3);

  for (size_t i = 0; i < 256; i++) {
    if (!(pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;

    uint64_t  pdpt_phys = pml4_virt[i] & PAGE_MASK;
    uint64_t *pdpt_virt = (uint64_t *)(hhdm + pdpt_phys);

    for (size_t j = 0; j < 512; j++) {
      if (!(pdpt_virt[j] & PAGE_FLAG_PRESENT))
        continue;
      if (pdpt_virt[j] & PAGE_FLAG_PS) // 1GB huge page — skip
        continue;

      uint64_t  pd_phys = pdpt_virt[j] & PAGE_MASK;
      uint64_t *pd_virt = (uint64_t *)(hhdm + pd_phys);

      for (size_t k = 0; k < 512; k++) {
        if (!(pd_virt[k] & PAGE_FLAG_PRESENT))
          continue;

        if (pd_virt[k] & PAGE_FLAG_PS) {
          // 2MB huge page — compute VA and check VMA.
          uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                        ((uint64_t)k << 21);
          struct vma *v = vma_find(vmas, va);
          if (v && (v->flags & MAP_SHARED)) {
            pd_virt[k] = 0;
            continue; // shared device page — do NOT free
          }
          uint64_t huge_phys = pd_virt[k] & PAGE_MASK;
          for (size_t p = 0; p < 512; p++)
            pmm_free_page((void *)(huge_phys + p * 4096));
          pd_virt[k] = 0;
          continue;
        }

        uint64_t  pt_phys = pd_virt[k] & PAGE_MASK;
        uint64_t *pt_virt = (uint64_t *)(hhdm + pt_phys);

        for (size_t l = 0; l < 512; l++) {
          if (!(pt_virt[l] & PAGE_FLAG_PRESENT))
            continue;

          uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                        ((uint64_t)k << 21) | ((uint64_t)l << 12);
          struct vma *v = vma_find(vmas, va);
          if (v && (v->flags & MAP_SHARED))
            continue; // shared mapping — do NOT free the physical frame

          pmm_free_page((void *)(pt_virt[l] & PAGE_MASK));
        }
        pmm_free_page((void *)pt_phys);
      }
      pmm_free_page((void *)pd_phys);
    }
    pmm_free_page((void *)pdpt_phys);
  }

  pmm_free_page((void *)cr3);
}
