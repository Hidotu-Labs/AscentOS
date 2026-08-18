
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../sched/sched.h"
#include "pmm.h"
#include "vma.h"
#include "vmm.h"
#include <stddef.h>
#include <stdint.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

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
  bool user_mode = (error_code & 0x4) != 0;
  bool write_fault = (error_code & 0x2) != 0;
  bool present_bit = (error_code & 0x1) != 0;
  bool exec_fault = (error_code & 0x10) != 0;


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
    uint64_t virt = cr2 & PAGE_MASK;

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

    if (*pte & PAGE_FLAG_RW) {
      // Stale TLB: Another thread in this process already broke CoW on this
      // page.
      vmm_flush_tlb(virt);
      return 0;
    }

    if (!(*pte & PAGE_FLAG_COW)) {
      // Not marked COW yet. Is it a MAP_PRIVATE VMA that's now writable?
      // (This can happen if it was first mapped read-only and then mprotected).
      spinlock_acquire(&current->mm->lock);
      struct vma *v = vma_find(&current->mm->vmas, cr2);
      if (v) {
        if ((v->flags & MAP_PRIVATE) && (v->prot & PROT_WRITE)) {
          // Writable private mappings may have been remapped read-only for
          // CoW. Read-only executable/file mappings must stay protected.
          *pte |= PAGE_FLAG_COW;
        }
      }
      spinlock_release(&current->mm->lock);
    }

    if (*pte & PAGE_FLAG_COW) {
      uint64_t old_phys = *pte & PAGE_MASK;
      uint16_t refs = pmm_get_ref((void *)old_phys);

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

    // Present page, write fault, no COW flag → genuine write protection
    // violation.
    if (user_mode) {
      klog_puts("[VMM] Write-protect fault at CR2=");
      klog_hex64(cr2);
      klog_puts(" RIP=");
      klog_hex64(regs->rip);
      klog_puts(" tid=");
      klog_uint64(current->tid);
      klog_puts(" RSP=");
      klog_hex64(regs->rsp);
      klog_puts("\n");
      return -1;
    }
  }

  // If we reach here for a PRESENT page, it means it's an unhandled protection
  // fault (e.g. executing a non-executable page, or a kernel RO violation).
  if (present_bit) {
    if (user_mode) {
      klog_puts("[VMM] Protection fault on present page at CR2=");
      klog_hex64(cr2);
      klog_puts(" RIP=");
      klog_hex64(regs->rip);
      klog_puts(" err=");
      klog_hex64(error_code);
      klog_puts("\n");
    }
    return -1;
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
      uint64_t old_end = vma->end;
      uint64_t new_start = cr2 & ~0xFFFULL;
      uint64_t prot = vma->prot;
      uint64_t flags = vma->flags;
      int fd = vma->fd;
      uint64_t offset = vma->offset;

      vma_remove(&current->mm->vmas, old_start, old_end);
      if (vma_add(&current->mm->vmas, new_start, old_end, prot, flags, fd,
                  offset, NULL, 0) != 0) {
        klog_puts("[VMM] Stack expansion failed (overlap?) for CR2=");
        klog_hex64(cr2);
        klog_puts("\n");
        vma_add(&current->mm->vmas, old_start, old_end, prot, flags, fd, offset,
                NULL, 0);
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

  uint64_t vma_prot = 0;
  uint64_t vma_flags = 0;
  int vma_fd = -1;
  uint64_t vma_offset = 0;
  uint64_t vma_file_size = 0;
  uint64_t vma_start = 0;
  uint64_t vma_end = 0;
  void *vma_file_node = NULL;
  if (vma) {
    vma_prot = vma->prot;
    vma_flags = vma->flags;
    vma_fd = vma->fd;
    (void)vma_fd; // captured for future use (e.g. close-on-exec logic)
    vma_offset = vma->offset;
    vma_file_size = vma->file_size;
    vma_start = vma->start;
    vma_end = vma->end;
    vma_file_node = vma->file_node;
  }
  spinlock_release(&current->mm->lock);

  if (!vma) {
    // No VMA covers this address — genuine segfault.
    if (user_mode) {
      klog_puts("[VMM] No VMA for CR2=");
      klog_hex64(cr2);
      klog_puts(" RIP=");
      klog_hex64(regs->rip);
      klog_puts(" tid=");
      klog_uint64(current->tid);
      klog_puts("\n[VMM] Active VMAs:\n");
      vma_dump(&current->mm->vmas);
      return -1;
    }

    if (!user_mode && cr2 <= USER_SPACE_LIMIT) {
      klog_puts("\n" KLOG_CLR_RED "[ FATAL ]" KLOG_CLR_RESET
                " KERNEL-MODE FAULT on user address\n");
      klog_puts("          CR2:  ");
      klog_hex64(cr2);
      klog_puts("\n");
      klog_puts("          RIP:  ");
      klog_hex64(regs->rip);
      klog_puts("\n");
      klog_puts("          TID:  ");
      klog_uint64(current->tid);
      klog_puts("\n\n");

      klog_puts("      RAX: ");
      klog_hex64(regs->rax);
      klog_puts(" RBX: ");
      klog_hex64(regs->rbx);
      klog_puts("\n");
      klog_puts("      RCX: ");
      klog_hex64(regs->rcx);
      klog_puts(" RDX: ");
      klog_hex64(regs->rdx);
      klog_puts("\n");
      klog_puts("      RSI: ");
      klog_hex64(regs->rsi);
      klog_puts(" RDI: ");
      klog_hex64(regs->rdi);
      klog_puts("\n");
      klog_puts("      RBP: ");
      klog_hex64(regs->rbp);
      klog_puts(" RSP: ");
      klog_hex64(regs->rsp);
      klog_puts("\n");
      klog_puts("      R8:  ");
      klog_hex64(regs->r8);
      klog_puts(" R9:  ");
      klog_hex64(regs->r9);
      klog_puts("\n");
      klog_puts("      R10: ");
      klog_hex64(regs->r10);
      klog_puts(" R11: ");
      klog_hex64(regs->r11);
      klog_puts("\n");
      klog_puts("      R12: ");
      klog_hex64(regs->r12);
      klog_puts(" R13: ");
      klog_hex64(regs->r13);
      klog_puts("\n");
      klog_puts("      R14: ");
      klog_hex64(regs->r14);
      klog_puts(" R15: ");
      klog_hex64(regs->r15);
      klog_puts("\n");

      process_do_exit(11); // SIGSEGV
    }
    klog_puts("[VMM] Segmentation fault at CR2=");
    klog_hex64(cr2);
    klog_puts("\n[VMM] Active VMAs:\n");
    vma_dump(&current->mm->vmas);
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
    uint64_t page_offset = (cr2 & ~0xFFFULL) - vma_start;
    uint64_t eff_file_size = (vma_file_size > 0) ? vma_file_size : (vma_end - vma_start);

    if (page_offset >= eff_file_size) {
      // 1. Pure BSS page (past eff_file_size) — allocate zero-filled page
      frame = pmm_alloc_page();
      if (!frame)
        return -1;
      memset(PHYS_TO_VIRT((uint64_t)frame), 0, 4096);
    } else if ((vma_flags & MAP_PRIVATE) && (vma_prot & PROT_WRITE)) {
      // 2. Private Writable file mapping (e.g. ELF .data / .got / partial BSS)
      //    Must allocate a private page so user writes never mutate shared page cache.
      frame = pmm_alloc_page();
      if (!frame)
        return -1;
      void *priv_virt = PHYS_TO_VIRT((uint64_t)frame);
      memset(priv_virt, 0, 4096);

      uint32_t valid_bytes = 4096;
      if (page_offset + 4096 > eff_file_size)
        valid_bytes = (uint32_t)(eff_file_size - page_offset);

      uint32_t file_offset = (uint32_t)(vma_offset + page_offset);
      vfs_page_t *cached = vfs_cache_lookup(node, file_offset);
      if (!cached) {
        // Clustered 128 KB read-ahead into VFS page cache (32 pages)
        uint32_t cluster_base = file_offset & ~0x1FFFFU;
        uint32_t cluster_end = cluster_base + 128 * 1024;
        if (cluster_end > node->length)
          cluster_end = node->length;

        for (uint32_t off = cluster_base; off < cluster_end; off += 4096) {
          vfs_page_t *p = vfs_cache_get_or_create(node, off);
          if (p)
            vfs_cache_put(node, p);
        }
        cached = vfs_cache_get_or_create(node, file_offset);
      } else {
        cached = vfs_cache_get_or_create(node, file_offset);
      }

      if (cached && cached->frame_phys) {
        memcpy(priv_virt, PHYS_TO_VIRT(cached->frame_phys), valid_bytes);
        vfs_cache_put(node, cached);
      }
    } else {
      // 3. Shared or Read-Only file mapping (e.g. ELF .text / .rodata)
      //    Use shared VFS page-cache frame directly.
      uint32_t file_offset = (uint32_t)(vma_offset + page_offset);
      vfs_page_t *cached = vfs_cache_lookup(node, file_offset);
      if (!cached) {
        // Clustered 64 KB read-ahead into VFS page cache
        uint32_t cluster_base = file_offset & ~0xFFFFU;
        uint32_t cluster_end = cluster_base + 64 * 1024;
        if (cluster_end > node->length)
          cluster_end = node->length;

        for (uint32_t off = cluster_base; off < cluster_end; off += 4096) {
          vfs_page_t *p = vfs_cache_get_or_create(node, off);
          if (p)
            vfs_cache_put(node, p);
        }
        cached = vfs_cache_get_or_create(node, file_offset);
      } else {
        cached = vfs_cache_get_or_create(node, file_offset);
      }

      if (cached && cached->frame_phys) {
        frame = (void *)cached->frame_phys;
        pmm_incref(frame);
        vfs_cache_put(node, cached);
      } else {
        frame = pmm_alloc_page();
        if (!frame)
          return -1;
        memset(PHYS_TO_VIRT((uint64_t)frame), 0, 4096);
      }

      // Proactive cluster mapping: map adjacent cached pages in the 64 KB window to avoid redundant faults
      uint64_t cluster_vstart = cr2 & ~0xFFFFULL;
      uint64_t pt_flags = dp_build_flags(vma_prot);
      for (int ci = 0; ci < 16; ci++) {
        uint64_t vpage = cluster_vstart + (uint64_t)(ci * 4096);
        if (vpage == (cr2 & ~0xFFFULL))
          continue;
        if (vpage < vma_start || vpage >= vma_end)
          continue;
        if (vmm_virt_to_phys((uint64_t *)target_cr3, vpage) != 0)
          continue;

        uint32_t foff = (uint32_t)(vma_offset + (vpage - vma_start));
        vfs_page_t *p = vfs_cache_lookup(node, foff);
        if (p && p->frame_phys) {
          pmm_incref((void *)p->frame_phys);
          if (!vmm_map_page((uint64_t *)target_cr3, vpage, p->frame_phys, pt_flags)) {
            pmm_decref((void *)p->frame_phys);
          }
          vfs_cache_put(node, p);
        }
      }
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
  if (!vmm_map_page((uint64_t *)target_cr3, cr2 & ~0xFFFULL, (uint64_t)frame,
                    flags)) {
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

// ─────────────────────────────────────────────────────────────────────────────
// vsyscall page — mapped at 0xFFFFFFFFFF600000 in every user process.
// Linux maps this fixed page so glibc/JVM can call gettimeofday, time, and
// getcpu without a full syscall.  HotSpot specifically hard-codes a read/call
// to 0xFFFFFFFFFF600800 (getcpu) to determine the CPU number for TLAB
// allocation.  Without this mapping the JVM crashes with SIGSEGV.
// ─────────────────────────────────────────────────────────────────────────────
#define VSYSCALL_BASE 0xFFFFFFFFFF600000ULL

static uint64_t vsyscall_page_phys = 0;

void vmm_init_vsyscall_page(void) {
  if (vsyscall_page_phys != 0)
    return; // already done

  void *page = pmm_alloc_page();
  if (!page)
    return;

  uint8_t *v = (uint8_t *)PHYS_TO_VIRT(page);
  // Fill entire page with 0xCC (int3) as a safe default
  for (int i = 0; i < 4096; i++)
    v[i] = 0xCC;

  // ── gettimeofday stub at offset 0x000 (syscall NR 96) ──
  // mov rax, 96; syscall; ret
  uint8_t gtod[] = {0x48,0xC7,0xC0,0x60,0x00,0x00,0x00, 0x0F,0x05, 0xC3};
  for (size_t i = 0; i < sizeof(gtod); i++)
    v[0x000 + i] = gtod[i];

  // ── time stub at offset 0x400 (syscall NR 201) ──
  // mov rax, 201; syscall; ret
  uint8_t t[] = {0x48,0xC7,0xC0,0xC9,0x00,0x00,0x00, 0x0F,0x05, 0xC3};
  for (size_t i = 0; i < sizeof(t); i++)
    v[0x400 + i] = t[i];

  // ── getcpu stub at offset 0x800 ──
  // getcpu(unsigned *cpu, unsigned *node, void *unused)
  // Always returns CPU 0, node 0.  HotSpot uses this to pick a TLAB shard.
  //   xor  eax, eax          ; return 0
  //   test rdi, rdi          ; if (cpu != NULL)
  //   jz   skip_cpu          ;
  //   mov  dword [rdi], 0    ;   *cpu = 0
  // skip_cpu:
  //   test rsi, rsi          ; if (node != NULL)
  //   jz   skip_node         ;
  //   mov  dword [rsi], 0    ;   *node = 0
  // skip_node:
  //   ret
  uint8_t gc[] = {
    0x31, 0xC0,                         // xor eax, eax
    0x48, 0x85, 0xFF,                   // test rdi, rdi
    0x74, 0x06,                         // jz  +6  (skip_cpu → land at test rsi)
    0xC7, 0x07, 0x00, 0x00, 0x00, 0x00,// mov dword [rdi], 0
    0x48, 0x85, 0xF6,                   // test rsi, rsi
    0x74, 0x06,                         // jz  +6  (skip_node → land at ret)
    0xC7, 0x06, 0x00, 0x00, 0x00, 0x00,// mov dword [rsi], 0
    0xC3,                               // ret
  };
  for (size_t i = 0; i < sizeof(gc); i++)
    v[0x800 + i] = gc[i];

  vsyscall_page_phys = (uint64_t)page;
}

void vmm_map_vsyscall_page(uint64_t *pml4) {
  if (vsyscall_page_phys == 0)
    return;
  // Map as user-accessible, readable, executable (no RW, no NX)
  vmm_map_page(pml4, VSYSCALL_BASE, vsyscall_page_phys, PAGE_FLAG_USER);
}


bool vmm_is_user_addr_range_valid(uint64_t addr, size_t size) {
  if (addr > USER_SPACE_LIMIT || (addr + size) > 0x800000000000ULL) {
    klog_puts("[VMM] Range validation failed: out of bounds\n");
    return false;
  }

  struct thread *current = sched_get_current();
  if (!current)
    return false;

  uint64_t start_page = addr & ~0xFFFULL;
  uint64_t end_page = (addr + size + 0xFFF) & ~0xFFFULL;

  // Acquire the MM lock once for the entire range scan instead of once per
  // page.  For a 1 MB buffer (256 pages) the old code did 256 lock/unlock
  // round-trips; now it does one.
  //
  // Additionally, when a VMA covers multiple pages we jump straight to its
  // end boundary rather than re-querying vma_find for every page inside it.
  // This reduces AVL tree lookups from O(pages) to O(distinct VMAs in range),
  // which for a normal process is typically 1-3 for any contiguous buffer.
  spinlock_acquire(&current->mm->lock);

  uint64_t page = start_page;
  while (page < end_page) {
    struct vma *v = vma_find(&current->mm->vmas, page);
    if (!v)
      v = vma_find_growdown(&current->mm->vmas, page, 8 * 1024 * 1024);

    if (!v) {
      klog_puts("[VMM] Range validation failed (No VMA) at ");
      klog_hex64(page);
      klog_puts(" in thread ");
      klog_uint64(current->tid);
      klog_puts("\n[VMM] Active VMAs:\n");
      vma_dump(&current->mm->vmas);
      spinlock_release(&current->mm->lock);
      return false;
    }

    if (v->prot == PROT_NONE) {
      klog_puts("[VMM] Range validation failed (PROT_NONE) at 0x");
      klog_uint64(page);
      klog_puts("\n");
      spinlock_release(&current->mm->lock);
      return false;
    }

    // Skip to the end of this VMA — every page inside it has the same prot.
    // Clamp to end_page so we don't overshoot on the last VMA.
    page = v->end < end_page ? v->end : end_page;
  }

  spinlock_release(&current->mm->lock);
  return true;
}


bool vmm_is_user_addr_range_writable(uint64_t addr, size_t size) {
  if (size == 0)
    return true;
  if (addr > USER_SPACE_LIMIT || size > 0x800000000000ULL - addr)
    return false;

  struct thread *current = sched_get_current();
  if (!current || !current->mm)
    return false;

  uint64_t start_page = addr & ~0xFFFULL;
  uint64_t end_page = (addr + size + 0xFFF) & ~0xFFFULL;

  spinlock_acquire(&current->mm->lock);

  uint64_t page = start_page;
  while (page < end_page) {
    struct vma *v = vma_find(&current->mm->vmas, page);
    if (!v)
      v = vma_find_growdown(&current->mm->vmas, page, 8 * 1024 * 1024);

    if (!v || !(v->prot & PROT_WRITE)) {
      spinlock_release(&current->mm->lock);
      return false;
    }

    page = v->end < end_page ? v->end : end_page;
  }

  spinlock_release(&current->mm->lock);
  return true;
}
