#include "vmm.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../sched/sched.h"
#include "pmm.h"
#include "vma.h"
#include <stddef.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

static uint64_t dp_build_flags(uint64_t prot) {
  if (prot == PROT_NONE)
    return 0;

  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & PROT_WRITE)
    flags |= PAGE_FLAG_RW;
  if (!(prot & PROT_EXEC))
    flags |= PAGE_FLAG_NX;
  return flags;
}

int vmm_handle_page_fault(uint64_t cr2, uint64_t error_code,
                          struct registers *regs) {
  (void)regs;
  bool user_mode   = (error_code & 0x4) != 0;
  bool write_fault = (error_code & 0x2) != 0;
  bool present_bit = (error_code & 0x1) != 0;

  struct thread *current = sched_get_current();
  if (!current)
    return -1; // kernel fault, no process context

  uint64_t target_cr3 = current->cr3;
  if (target_cr3 == 0) {
    __asm__ volatile("mov %%cr3, %0" : "=r"(target_cr3));
    target_cr3 &= 0xFFFFFFFFFFFFF000ULL;
  }

  if (present_bit && write_fault) {
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(target_cr3);
    uint64_t  virt = cr2 & PAGE_MASK;

    if (!(pml4[(virt >> 39) & 511] & PAGE_FLAG_PRESENT))
      return -1;
    uint64_t *pdpt =
        (uint64_t *)PHYS_TO_VIRT(pml4[(virt >> 39) & 511] & PAGE_MASK);

    if (!(pdpt[(virt >> 30) & 511] & PAGE_FLAG_PRESENT))
      return -1;
    if (pdpt[(virt >> 30) & 511] & PAGE_FLAG_PS)
      return -1; 

    uint64_t *pd =
        (uint64_t *)PHYS_TO_VIRT(pdpt[(virt >> 30) & 511] & PAGE_MASK);
    if (!(pd[(virt >> 21) & 511] & PAGE_FLAG_PRESENT))
      return -1;
    if (pd[(virt >> 21) & 511] & PAGE_FLAG_PS)
      return -1; 

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[(virt >> 21) & 511] & PAGE_MASK);
    uint64_t *pte = &pt[(virt >> 12) & 511];
    if (!(*pte & PAGE_FLAG_PRESENT))
      return -1;

    if (*pte & PAGE_FLAG_COW) {
      uint64_t  old_phys = *pte & PAGE_MASK;
      uint16_t  refs     = pmm_get_ref((void *)old_phys);

      if (refs > 1) {
        // Multiple owners — make a private copy.
        void *new_phys = pmm_alloc_page();
        if (!new_phys)
          return -1;

        memcpy(PHYS_TO_VIRT((uint64_t)new_phys), PHYS_TO_VIRT(old_phys),
               PAGE_SIZE);

        *pte = ((uint64_t)new_phys & PAGE_MASK) |
               (*pte & ~PAGE_MASK & ~PAGE_FLAG_COW) | PAGE_FLAG_RW;
        pmm_decref((void *)old_phys);
      } else {
        // Sole owner — just make it writable in place.
        *pte &= ~PAGE_FLAG_COW;
        *pte |= PAGE_FLAG_RW;
      }

      vmm_flush_tlb(virt);
      return 0; // fault handled
    }
  }

  // A present page with no CoW flag → real protection violation.
  if (present_bit)
    return -1;

  // ---- VMA-based demand paging --------------------------------------------

  if (!current || !current->mm)
    return -1;

  spinlock_acquire(&current->mm->lock);

  struct vma *vma = vma_find(&current->mm->vmas, cr2);

  if (!vma) {
    // Try automatic stack growth (GROWSDOWN VMA within 8 MB).
    vma = vma_find_growdown(&current->mm->vmas, cr2, 8 * 1024 * 1024);
    if (vma) {
      uint64_t old_start = vma->start;
      uint64_t old_end   = vma->end;
      uint64_t new_start = cr2 & ~0xFFFULL;
      uint64_t prot      = vma->prot;
      uint64_t flags     = vma->flags;
      int      fd        = vma->fd;
      uint64_t offset    = vma->offset;

      vma_remove(&current->mm->vmas, old_start, old_end);
      if (vma_add(&current->mm->vmas, new_start, old_end, prot, flags, fd,
                  offset, NULL) != 0) {
        klog_puts("[VMM] Stack expansion failed (overlap?) for CR2=");
        klog_hex64(cr2);
        klog_puts("\n");
        vma_add(&current->mm->vmas, old_start, old_end, prot, flags, fd,
                offset, NULL);
        vma = NULL;
      } else {
        vma = vma_find(&current->mm->vmas, cr2);
      }
    } else {
      struct vma *potential =
          vma_find_growdown(&current->mm->vmas, cr2, 1024ULL * 1024 * 1024);
      if (potential) {
        klog_puts("[VMM] Rejected stack growth: CR2=");
        klog_hex64(cr2);
        klog_puts(" is too far below VMA ");
        klog_hex64(potential->start);
        klog_puts("\n");
      }
    }
  }

  uint64_t vma_prot      = 0;
  int      vma_fd        = -1;
  uint64_t vma_offset    = 0;
  uint64_t vma_start     = 0;
  uint64_t vma_end       = 0;
  void    *vma_file_node = NULL;
  if (vma) {
    vma_prot      = vma->prot;
    vma_fd        = vma->fd;
    (void)vma_fd; // captured for future use (e.g. close-on-exec logic)
    vma_offset    = vma->offset;
    vma_start     = vma->start;
    vma_end       = vma->end;
    vma_file_node = vma->file_node;
  }
  spinlock_release(&current->mm->lock);

  if (!vma) {
    // No VMA covers this address — genuine segfault.
    if (!user_mode && cr2 <= USER_SPACE_LIMIT) {
      klog_puts("\n[VMM] KERNEL-MODE FAULT on user address CR2=");
      klog_hex64(cr2);
      klog_puts(" RIP=");
      klog_hex64(regs->rip);
      klog_puts(" thread=");
      klog_uint64(current->tid);
      klog_puts("\n[VMM] This usually indicates a missing "
                "vmm_is_user_addr_range_valid() check in a syscall.\n");
      process_do_exit(11); // SIGSEGV
    }
    klog_puts("[VMM] Segmentation fault at CR2=");
    klog_hex64(cr2);
    klog_puts("\n");
    return -1;
  }

  // PROT_NONE enforcement — reserves address space but forbids all access.
  if (vma_prot == PROT_NONE) {
    if (user_mode)
      klog_puts("[VMM] PROT_NONE access violation\n");
    return -1;
  }

  // Write to read-only VMA.
  if (write_fault && !(vma_prot & PROT_WRITE)) {
    if (user_mode)
      klog_puts("[VMM] Write to read-only VMA\n");
    return -1;
  }

  void *frame = NULL;

  vfs_node_t *node = (vfs_node_t *)vma_file_node;
  if (node && (node->flags & FS_TYPE_MASK) == FS_FILE) {
    // ---- File-backed demand paging with clustered read-ahead ---------------
    uint64_t page_offset  = (cr2 & ~0xFFFULL) - vma_start;
    uint32_t file_offset  = (uint32_t)(vma_offset + page_offset);

    // 1. Try page cache first.
    vfs_page_t *cached = vfs_cache_lookup(node, file_offset);
    if (cached) {
      frame = (void *)cached->frame_phys;
    } else {
      // 2. Cache miss: read a 64 KB cluster to maximise disk throughput.
      uint32_t cluster_base = file_offset & ~0xFFFFU; // 64 KB aligned
      uint32_t cluster_size = 64 * 1024;
      if (cluster_base + cluster_size > node->length)
        cluster_size = (node->length > cluster_base)
                           ? (node->length - cluster_base)
                           : 0;

      for (uint32_t off = 0; off < cluster_size; off += 4096) {
        uint32_t cur_off = cluster_base + off;
        if (vfs_cache_lookup(node, cur_off))
          continue;

        void *nf = pmm_alloc_page();
        if (!nf)
          break;

        uint32_t to_read = (node->length - cur_off >= 4096)
                               ? 4096
                               : (node->length - cur_off);
        if (to_read > 0) {
          vfs_read(node, cur_off, to_read,
                   (uint8_t *)PHYS_TO_VIRT((uint64_t)nf));
          if (to_read < 4096)
            memset((uint8_t *)PHYS_TO_VIRT((uint64_t)nf) + to_read, 0,
                   4096 - to_read);
        } else {
          memset(PHYS_TO_VIRT((uint64_t)nf), 0, 4096);
        }

        vfs_cache_insert(node, cur_off, (uint64_t)nf);
      }

      cached = vfs_cache_lookup(node, file_offset);
      if (cached)
        frame = (void *)cached->frame_phys;
    }

    if (!frame) {
      // Past EOF or OOM — map a zero page.
      frame = pmm_alloc_page();
      if (!frame)
        return -1;
      memset(PHYS_TO_VIRT((uint64_t)frame), 0, 4096);
    }

    // 3. Proactive cluster mapping — map any already-cached pages in the
    //    same 64 KB window to avoid redundant faults for the same library.
    uint64_t cluster_vstart = cr2 & ~0xFFFFULL;
    uint64_t pt_flags       = dp_build_flags(vma_prot);

    for (int ci = 0; ci < 16; ci++) {
      uint64_t vpage = cluster_vstart + (uint64_t)(ci * 4096);
      if (vpage == (cr2 & ~0xFFFULL))
        continue; // handled below
      if (vpage < vma_start || vpage >= vma_end)
        continue;
      if (vmm_virt_to_phys((uint64_t *)target_cr3, vpage) != 0)
        continue;

      uint32_t foff = (uint32_t)(vma_offset + (vpage - vma_start));
      vfs_page_t *p = vfs_cache_lookup(node, foff);
      if (p)
        vmm_map_page((uint64_t *)target_cr3, vpage, p->frame_phys, pt_flags);
    }

  } else {
    // ---- Anonymous zero-fill-on-demand -------------------------------------
    frame = pmm_alloc_page();
    if (!frame) {
      klog_puts("[VMM] OOM during demand paging!\n");
      if (user_mode) {
        sched_terminate_thread(current->tid);
        return 0;
      }
      return -1;
    }
    memset(PHYS_TO_VIRT((uint64_t)frame), 0, 4096);
  }

  // ---- Map the faulting page ----------------------------------------------

  uint64_t flags = dp_build_flags(vma_prot);

  if (!vmm_map_page((uint64_t *)target_cr3, cr2 & ~0xFFFULL,
                    (uint64_t)frame, flags)) {
    pmm_free_page(frame);
    klog_puts("[VMM] Fatal PT alloc failure in paging engine\n");
    if (user_mode) {
      sched_terminate_thread(current->tid);
      return 0;
    }
    return -1;
  }

  return 0;
}

void vmm_map_signal_trampoline(uint64_t *pml4) {
  extern uint64_t signal_trampoline_phys;
  if (signal_trampoline_phys == 0)
    return;
  vmm_map_page(pml4, 0x00007FFFFFFFF000ULL, signal_trampoline_phys,
               PAGE_FLAG_USER);
}

bool vmm_is_user_addr_range_valid(uint64_t addr, size_t size) {
  if (addr > USER_SPACE_LIMIT ||
      (addr + size) > 0x800000000000ULL) {
    klog_puts("[VMM] Range validation failed: out of bounds\n");
    return false;
  }

  struct thread *current = sched_get_current();
  if (!current)
    return false;

  uint64_t start_page = addr & ~0xFFFULL;
  uint64_t end_page   = (addr + size + 0xFFF) & ~0xFFFULL;

  for (uint64_t page = start_page; page < end_page; page += 0x1000) {
    spinlock_acquire(&current->mm->lock);
    struct vma *v = vma_find(&current->mm->vmas, page);
    if (!v)
      v = vma_find_growdown(&current->mm->vmas, page, 8 * 1024 * 1024);

    if (v) {
      if (v->prot == PROT_NONE) {
        klog_puts("[VMM] Range validation failed (PROT_NONE) at 0x");
        klog_uint64(page);
        klog_puts("\n");
        spinlock_release(&current->mm->lock);
        return false;
      }
      spinlock_release(&current->mm->lock);
    } else {
      klog_puts("[VMM] Range validation failed (No VMA) at ");
      klog_hex64(page);
      klog_puts(" in thread ");
      klog_uint64(current->tid);
      klog_puts("\n");
      spinlock_release(&current->mm->lock);
      return false;
    }
  }

  return true;
}
