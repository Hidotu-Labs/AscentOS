// Memory Management Syscalls: mmap, munmap, brk
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

// Linux mmap constants
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((uint64_t)-1)

// Errno constants
#define E_INVAL ((uint64_t)-22)
#define E_NOMEM ((uint64_t)-12)
#define E_BADF ((uint64_t)-9)

// Helpers

#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define HHDM_OFFSET pmm_get_hhdm_offset()
#define PML4_PHYS_TO_VIRT(p) ((uint64_t *)(((uint64_t)(p)) + HHDM_OFFSET))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// User-space pointer validation: reject anything above canonical user range.
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL
static inline bool is_user_pointer(uint64_t addr) {
  return addr <= USER_ADDR_MAX;
}

static uint64_t build_page_flags(uint64_t prot) {
  // PROT_NONE → no flags at all (page must stay non-present).
  // On x86-64 there is no "read disable" bit; PRESENT alone grants reads.
  if (prot == PROT_NONE)
    return 0;

  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & PROT_WRITE)
    flags |= PAGE_FLAG_RW;
  if (!(prot & PROT_EXEC))
    flags |= PAGE_FLAG_NX;
  return flags;
}

// Anonymous mapping
#define MMAP_REGION_BASE 0x7F0000000000ULL
#define MMAP_REGION_LIMIT 0x7FF000000000ULL

// INTERNAL HELPERS

static void safe_unmap_and_free(uint64_t *pml4, uint64_t va, uint64_t phys,
                                bool free_phys, const char *ctx) {
  (void)ctx;
  // Step 1: remove the PTE.  After this the frame is unreachable via this VA.
  vmm_unmap_page(pml4, va);

  // Step 2: only now is it safe to recycle the frame.
  if (free_phys && phys != 0) {
    pmm_free((void *)phys);
  }
}

// teardown_range:
//   Unmap [base, base+len) and free anonymous private frames.
//   Used by both MAP_FIXED pre-teardown and munmap proper.
//   Does NOT touch the VMA list — callers manage that themselves.
static void teardown_range(uint64_t *pml4, struct thread *t, uint64_t base,
                           uint64_t len, const char *ctx) {
  uint64_t end = base + len;

  for (uint64_t va = base; va < end; va += PAGE_SIZE) {
    uint64_t phys = vmm_virt_to_phys(pml4, va);
    if (phys == 0)
      continue; // Already unmapped, nothing to do.

    phys = PAGE_ALIGN_DOWN(phys); // Strip low flag bits VMM may leave set.

    // Only free frames we own: anonymous private mappings.
    bool free_phys = false;
    if (t && t->mm) {
      struct vma *v = vma_find(&t->mm->vmas, va);
      if (v && v->fd == -1 && (v->flags & MAP_PRIVATE) &&
          (v->flags & MAP_ANONYMOUS)) {
        free_phys = true;
      }
    }

    safe_unmap_and_free(pml4, va, phys, free_phys, ctx);
  }
}

// sys_mmap
// Linux ABI: mmap(addr, length, prot, flags, fd, offset)
//   rdi=addr  rsi=length  rdx=prot  r10=flags  r8=fd  r9=offset
uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags,
                  uint64_t fd, uint64_t offset) {
  (void)offset;

  /*
    klog_puts("[MMAP] addr=");
    klog_uint64(addr);
    klog_puts(" len=");
    klog_uint64(length);
    klog_puts(" prot=0x");
    klog_uint64(prot);
    klog_puts(" flags=0x");
    klog_uint64(flags);
    klog_puts("\n");
  */

  // Validate flags
  bool is_shared = (flags & MAP_SHARED) != 0;
  bool is_private = (flags & MAP_PRIVATE) != 0;

  if (!is_shared && !is_private) {
    return E_INVAL;
  }
  if (is_shared && is_private) {
    return E_INVAL;
  }
  if (length == 0) {
    return E_INVAL;
  }

  // Determine virtual address
  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  struct thread *current_thread = sched_get_current();


  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t vaddr = 0;

  if (flags & MAP_FIXED) {
    if (addr == 0 || (addr & (PAGE_SIZE - 1))) {
      return E_INVAL;
    }
    if (!is_user_pointer(addr) || !is_user_pointer(addr + aligned_len - 1)) {
      return E_INVAL;
    }
    vaddr = addr;

    // Tear down any existing mappings in the target range
    teardown_range(pml4, current_thread, vaddr, aligned_len,
                   "MAP_FIXED teardown");
    if (current_thread && current_thread->mm) {
      spinlock_acquire(&current_thread->mm->lock);
      vma_remove(&current_thread->mm->vmas, vaddr, vaddr + aligned_len);
      spinlock_release(&current_thread->mm->lock);
    }
  } else {
    // Non-fixed: allocate dynamically utilizing AVL Interval Gap Finding
    if (current_thread && current_thread->mm) {
      spinlock_acquire(&current_thread->mm->lock);

      // Try addr as a hint if provided and aligned
      if (addr != 0 && (addr & (PAGE_SIZE - 1)) == 0 &&
          is_user_pointer(addr) && is_user_pointer(addr + aligned_len - 1) &&
          addr >= MMAP_REGION_BASE && addr + aligned_len <= MMAP_REGION_LIMIT) {
        if (!vma_find_overlap(&current_thread->mm->vmas, addr, addr + aligned_len)) {
          vaddr = addr;
        }
      }

      if (vaddr == 0) {
        vaddr = vma_find_gap(&current_thread->mm->vmas, aligned_len,
                             MMAP_REGION_BASE, MMAP_REGION_LIMIT);
      }
      spinlock_release(&current_thread->mm->lock);
    }

    if (vaddr == 0) {
      // Fallback to legacy allocator if AVL gap finding fails or thread context
      // missing
      vaddr = mm_alloc_mmap_region(aligned_len);
    }

    if (vaddr == 0 || vaddr + aligned_len > MMAP_REGION_LIMIT) {
      return E_NOMEM;
    }
  }

  // File-backed mapping
  if (!(flags & MAP_ANONYMOUS) && (int64_t)fd != -1) {
    if (!current_thread || fd >= MAX_FDS || !current_thread->fds[fd]) {
      return E_BADF;
    }
    vfs_node_t *node = current_thread->fds[fd];
    if (!node->mmap) {
      return E_INVAL;
    }

    // Pass MAP_FIXED to internal handler to ensure it respects our vaddr
    uint64_t result =
        node->mmap(node, vaddr, length, prot, flags | MAP_FIXED, offset);
    if (result == MAP_FAILED || result == (uint64_t)-1)
      return E_NOMEM;

    if (current_thread && current_thread->mm) {
      spinlock_acquire(&current_thread->mm->lock);
      int vma_idx =
          vma_add(&current_thread->mm->vmas, result, result + aligned_len, prot,
                  flags, (int)fd, offset, node);
      spinlock_release(&current_thread->mm->lock);
      if (vma_idx < 0) {
        return E_NOMEM;
      }
    }

    return result;
  }

  // Reject non-anonymous mappings with no fd
  if (!(flags & MAP_ANONYMOUS)) {
    return E_BADF;
  }

  // Anonymous mapping
  // (Address determination already handled above)

  // PROT_NONE shortcut
  // On x86-64 there is no "read-disable" bit; any PRESENT page is readable.
  // For a true PROT_NONE mapping we must NOT create any PTEs.  We only
  // record the VMA so the address range is reserved, but leave the pages
  // non-present.  Any access will page-fault, which is the correct
  // PROT_NONE behaviour.

  // Demand Paging: register VMA only, no physical allocation
  // For anonymous mappings we simply record the VMA.  Physical frames are
  // allocated lazily by vmm_handle_page_fault() on first access.  This
  // dramatically reduces memory consumption for large mappings that are
  // only partially touched (e.g. musl's mmap-backed malloc arenas).

  // Register VMA
  if (current_thread && current_thread->mm) {
    spinlock_acquire(&current_thread->mm->lock);
    int vma_idx = vma_add(&current_thread->mm->vmas, vaddr, vaddr + aligned_len,
                          prot, flags, -1, 0, NULL);
    if (vma_idx < 0) {
      spinlock_release(&current_thread->mm->lock);
      return E_NOMEM;
    }

    // Update the mmap bump pointer if we were using the old-style allocator
    // range
    current_thread->mm->mmap_next_addr =
        MAX(current_thread->mm->mmap_next_addr, vaddr + aligned_len);
    spinlock_release(&current_thread->mm->lock);
  }

  return vaddr;
}

// sys_munmap
// Linux ABI: munmap(addr, length)
//   rdi=addr  rsi=length
uint64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  // Validate arguments
  if (addr == 0 || length == 0)
    return E_INVAL;
  if (addr & (PAGE_SIZE - 1))
    return E_INVAL; // addr must be page-aligned (POSIX).
  if (!is_user_pointer(addr) || !is_user_pointer(addr + length - 1))
    return E_INVAL;

  struct thread *current = sched_get_current();
  if (!current)
    return E_INVAL;


  // Linux munmap is permissive: it unmaps whatever is in the range.
  // It does not require a VMA to exist at 'addr'.

  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  uint64_t *pml4 = vmm_get_active_pml4();

  spinlock_acquire(&current->mm->lock);
  struct vma *first_vma = vma_find_overlap(&current->mm->vmas, addr, addr + aligned_len);
  bool is_shared = first_vma && (first_vma->flags & MAP_SHARED);
  spinlock_release(&current->mm->lock);


  // Unmap and free
  // teardown_range handles the unmap-before-free ordering and guards.
  // It consults the VMA list to decide whether each frame is owned by us.
  teardown_range(pml4, current, addr, aligned_len, "sys_munmap");

  // Remove VMAs
  spinlock_acquire(&current->mm->lock);
  vma_remove(&current->mm->vmas, addr, addr + aligned_len);
  vma_merge_adjacent(&current->mm->vmas);
  spinlock_release(&current->mm->lock);

  return 0;
}

// sys_brk  (unchanged from original — included for completeness)
// Linux ABI: brk(addr)  —  rdi=addr
// Returns the current/new program break.
static uint64_t sys_brk(uint64_t addr, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return 0;


  // Linux ABI: brk(0) returns current break.
  spinlock_acquire(&current->mm->lock);
  if (addr == 0) {
    uint64_t ret = current->mm->brk_current;
    spinlock_release(&current->mm->lock);
    return ret;
  }

  // Check bounds.
  if (!is_user_pointer(addr)) {
    uint64_t ret = current->mm->brk_current;
    spinlock_release(&current->mm->lock);
    return ret;
  }

  uint64_t *pml4 = vmm_get_active_pml4();

  if (addr > current->mm->brk_current) {
    uint64_t old_end = PAGE_ALIGN_UP(current->mm->brk_current);
    uint64_t new_end = PAGE_ALIGN_UP(addr);

    if (new_end > old_end) {
      if (vma_find_overlap(&current->mm->vmas, old_end, new_end)) {
        spinlock_release(&current->mm->lock);
        return current->mm->brk_current; // Overlap with existing VMA
      }
      if (vma_add(&current->mm->vmas, old_end, new_end, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, NULL) < 0) {
        spinlock_release(&current->mm->lock);
        return current->mm->brk_current;
      }
    }

    current->mm->brk_current = addr;

  } else if (addr < current->mm->brk_current && addr >= current->mm->brk_base) {
    uint64_t old_end = PAGE_ALIGN_UP(current->mm->brk_current);
    uint64_t new_end = PAGE_ALIGN_UP(addr);

    for (uint64_t page = new_end; page < old_end; page += PAGE_SIZE) {
      uint64_t phys = vmm_virt_to_phys(pml4, page);
      if (phys != 0) {
        phys = PAGE_ALIGN_DOWN(phys);
        safe_unmap_and_free(pml4, page, phys, true, "sys_brk shrink");
      }
    }

    vma_remove(&current->mm->vmas, new_end, old_end);
    current->mm->brk_current = addr;
  }

  uint64_t ret = current->mm->brk_current;
  spinlock_release(&current->mm->lock);

  return ret;
}

// sys_mprotect  (unchanged from original — included for completeness)
// Linux ABI: mprotect(addr, len, prot)
static uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (addr & (PAGE_SIZE - 1))
    return E_INVAL;
  if (len == 0)
    return 0;

  uint64_t aligned_len = PAGE_ALIGN_UP(len);
  uint64_t *pml4 = vmm_get_active_pml4();

  struct thread *current = sched_get_current();
  if (!current)
    return E_INVAL;

  spinlock_acquire(&current->mm->lock);

  for (uint64_t va = addr; va < addr + aligned_len; va += PAGE_SIZE) {
    uint64_t phys = vmm_virt_to_phys(pml4, va);
    if (phys == 0)
      continue;

    uint64_t new_flags = build_page_flags(prot);
    vmm_unmap_page(pml4, va);
    if (!vmm_map_page(pml4, va, PAGE_ALIGN_DOWN(phys), new_flags)) {
      spinlock_release(&current->mm->lock);
      return E_NOMEM;
    }
  }

  // Synchronize the VMA tree so that syscall validation
  // (vmm_is_user_addr_range_valid) sees the updated protection bits.
  vma_mprotect(&current->mm->vmas, addr, addr + aligned_len, prot);
  vma_merge_adjacent(&current->mm->vmas);

  spinlock_release(&current->mm->lock);

  return 0;
}

// sys_mremap
// Linux ABI: mremap(old_addr, old_size, new_size, flags, [new_addr])
//   rdi=old_addr  rsi=old_size  rdx=new_size  r10=flags  r8=new_addr
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

static uint64_t sys_mremap(uint64_t old_addr, uint64_t old_size,
                           uint64_t new_size, uint64_t flags,
                           uint64_t new_addr_hint, uint64_t a5) {
  (void)new_addr_hint;
  (void)a5;

  if (old_addr == 0 || old_size == 0 || new_size == 0)
    return E_INVAL;
  if (old_addr & (PAGE_SIZE - 1))
    return E_INVAL;
  if (!is_user_pointer(old_addr))
    return E_INVAL;

  // We only support MREMAP_MAYMOVE for now (the common musl realloc path).
  // MREMAP_FIXED is rarely used and complex to implement.
  if (flags & MREMAP_FIXED)
    return E_INVAL;

  struct thread *current = sched_get_current();
  if (!current)
    return E_INVAL;

  uint64_t aligned_old = PAGE_ALIGN_UP(old_size);
  uint64_t aligned_new = PAGE_ALIGN_UP(new_size);
  uint64_t *pml4 = vmm_get_active_pml4();

  spinlock_acquire(&current->mm->lock);

  // Shrink: just unmap the tail pages and update the VMA.
  if (aligned_new <= aligned_old) {
    if (aligned_new < aligned_old) {
      uint64_t trim_base = old_addr + aligned_new;
      uint64_t trim_len = aligned_old - aligned_new;
      teardown_range(pml4, current, trim_base, trim_len, "mremap shrink");
      vma_remove(&current->mm->vmas, trim_base, trim_base + trim_len);
    }
    spinlock_release(&current->mm->lock);
    return old_addr;
  }

  // Grow: try in-place first — check if pages right after old region are free.
  uint64_t grow_base = old_addr + aligned_old;
  uint64_t grow_len = aligned_new - aligned_old;
  bool can_grow_inplace = true;

  // Check that the expansion area doesn't overlap any existing VMA.
  if (vma_find_overlap(&current->mm->vmas, grow_base, grow_base + grow_len)) {
    can_grow_inplace = false;
  }

  // Also verify no existing page mappings in the expansion zone.
  if (can_grow_inplace) {
    for (uint64_t va = grow_base; va < grow_base + grow_len; va += PAGE_SIZE) {
      if (vmm_virt_to_phys(pml4, va) != 0) {
        can_grow_inplace = false;
        break;
      }
    }
  }

  // Look up the original VMA to get its protection/flags.
  struct vma *orig_vma = vma_find(&current->mm->vmas, old_addr);
  if (!orig_vma) {
    spinlock_release(&current->mm->lock);
    return E_INVAL;
  }
  uint64_t prot = orig_vma->prot;
  uint64_t vma_flags = orig_vma->flags;
  uint64_t page_flags = build_page_flags(prot);

  if (can_grow_inplace) {
    // Allocate and map the new pages.
    for (uint64_t va = grow_base; va < grow_base + grow_len; va += PAGE_SIZE) {
      void *phys = pmm_alloc();
      if (!phys) {
        spinlock_release(&current->mm->lock);
        return E_NOMEM; // OOM — leave partial state; not ideal but safe.
      }
      if (!vmm_map_page(pml4, va, (uint64_t)phys, page_flags)) {
        pmm_free(phys);
        spinlock_release(&current->mm->lock);
        return E_NOMEM;
      }
      void *kva = (void *)((uint64_t)phys + HHDM_OFFSET);
      memset(kva, 0, PAGE_SIZE);
    }
    // Extend the VMA to cover the new range.
    vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);
    vma_add(&current->mm->vmas, old_addr, old_addr + aligned_new, prot,
            vma_flags, -1, 0, NULL);
    spinlock_release(&current->mm->lock);
    return old_addr;
  }

  // Cannot grow in-place.  If MAYMOVE is allowed, allocate a new region,
  // copy old data, and unmap the old region.
  if (!(flags & MREMAP_MAYMOVE)) {
    spinlock_release(&current->mm->lock);
    return E_NOMEM; // Actually Linux returns ENOMEM here if it can't grow in-place and MAYMOVE not set.
  }

  uint64_t new_addr = vma_find_gap(&current->mm->vmas, aligned_new,
                                   MMAP_REGION_BASE, MMAP_REGION_LIMIT);
  if (new_addr == 0 || new_addr + aligned_new > MMAP_REGION_LIMIT) {
    spinlock_release(&current->mm->lock);
    return E_NOMEM;
  }

  // Allocate and map new pages.
  for (uint64_t i = 0; i < aligned_new / PAGE_SIZE; i++) {
    void *phys = pmm_alloc();
    if (!phys) {
      spinlock_release(&current->mm->lock);
      return E_NOMEM;
    }
    if (!vmm_map_page(pml4, new_addr + i * PAGE_SIZE, (uint64_t)phys,
                      page_flags)) {
      pmm_free(phys);
      spinlock_release(&current->mm->lock);
      return E_NOMEM;
    }
    void *kva = (void *)((uint64_t)phys + HHDM_OFFSET);
    memset(kva, 0, PAGE_SIZE);
  }

  // Copy old data to new location (page by page through HHDM).
  for (uint64_t off = 0; off < aligned_old; off += PAGE_SIZE) {
    uint64_t old_phys = vmm_virt_to_phys(pml4, old_addr + off);
    uint64_t new_phys = vmm_virt_to_phys(pml4, new_addr + off);
    if (old_phys && new_phys) {
      void *src = (void *)(PAGE_ALIGN_DOWN(old_phys) + HHDM_OFFSET);
      void *dst = (void *)(PAGE_ALIGN_DOWN(new_phys) + HHDM_OFFSET);
      memcpy(dst, src, PAGE_SIZE);
    }
  }

  // Tear down old mapping.
  teardown_range(pml4, current, old_addr, aligned_old, "mremap move");
  vma_remove(&current->mm->vmas, old_addr, old_addr + aligned_old);

  // Register new VMA.
  vma_add(&current->mm->vmas, new_addr, new_addr + aligned_new, prot, vma_flags,
          -1, 0, NULL);

  current->mm->mmap_next_addr =
      MAX(current->mm->mmap_next_addr, new_addr + aligned_new);

  spinlock_release(&current->mm->lock);
  return new_addr;
}

static uint64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)addr;
  (void)len;
  (void)advice;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0; // Success stub
}

// Public API

void syscall_register_mm(void) {
  syscall_register(SYS_MMAP, sys_mmap);
  syscall_register(SYS_MUNMAP, sys_munmap);
  syscall_register(SYS_MREMAP, sys_mremap);
  syscall_register(SYS_MADVISE, sys_madvise);
  syscall_register(SYS_BRK, sys_brk);
  syscall_register(SYS_MPROTECT, sys_mprotect);
}
// Called from process_exec when loading a new process image.
void mm_reset_mmap_state(struct thread *t) {
  if (!t || !t->mm)
    return;
  t->mm->mmap_next_addr = MMAP_REGION_BASE;
}

// Allocate a virtual address range from the mmap region for device mmap
// handlers (e.g. framebuffer, DMA).  Returns the base VA, or 0 on failure.
uint64_t mm_alloc_mmap_region(uint64_t length) {
  if (length == 0)
    return 0;

  struct thread *current = sched_get_current();
  if (!current || !current->mm)
    return 0;

  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  spinlock_acquire(&current->mm->lock);
  if (current->mm->mmap_next_addr + aligned_len > MMAP_REGION_LIMIT) {
    spinlock_release(&current->mm->lock);
    return 0;
  }

  uint64_t vaddr = current->mm->mmap_next_addr;
  current->mm->mmap_next_addr += aligned_len;
  spinlock_release(&current->mm->lock);
  return vaddr;
}
