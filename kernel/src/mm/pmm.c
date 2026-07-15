#include "mm/pmm.h"
#include "hal/hal.h"
#include "console/klog.h"
#include "lib/list.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_ORDER 20

struct buddy_zone {
  struct list_head free_list[MAX_ORDER];
  spinlock_t lock;
};

struct buddy_block {
  struct list_head node;
  size_t order;
};

static struct buddy_zone b_zone;
static spinlock_t pmm_lock = SPINLOCK_INIT;

static uint8_t *bitmap = NULL;
static uint8_t *managed_bitmap = NULL;
static uint16_t *refcounts = NULL; // Array of refcounts per page
static size_t bitmap_size = 0;     // in bytes
static uint64_t lowest_page = 0xFFFFFFFFFFFFFFFF;
static uint64_t highest_page = 0;
static uint64_t page_count = 0;
static uint64_t usable_memory = 0;
static uint64_t total_memory = 0;
static uint64_t physical_memory_offset = 0;
static struct limine_memmap_response *internal_memmap = NULL;

static inline void bitmap_clear(uint8_t *bm, size_t bit) {
  bm[bit / 8] &= ~(1 << (bit % 8));
}

static inline int bitmap_test(uint8_t *bm, size_t bit) {
  return (bm[bit / 8] & (1 << (bit % 8))) != 0;
}

static inline void bitmap_set(uint8_t *bm, size_t bit) {
  bm[bit / 8] |= (1 << (bit % 8));
}

static void bitmap_set_range(uint8_t *bm, size_t start_bit, size_t count) {
  if (start_bit < lowest_page)
    return;
  if (start_bit >= highest_page)
    return;
  if (start_bit + count > highest_page)
    count = highest_page - start_bit;

  size_t idx = start_bit - lowest_page;
  size_t end_idx = idx + count;

  while (idx < end_idx && (idx % 8) != 0) {
    bitmap_set(bm, idx);
    idx++;
  }

  if (end_idx > idx) {
    size_t full_bytes = (end_idx - idx) / 8;
    if (full_bytes > 0) {
      memset(&bm[idx / 8], 0xFF, full_bytes);
      idx += full_bytes * 8;
    }
  }

  while (idx < end_idx) {
    bitmap_set(bm, idx);
    idx++;
  }
}

static void bitmap_clear_range(uint8_t *bm, size_t start_bit, size_t count) {
  if (start_bit < lowest_page)
    return;
  if (start_bit >= highest_page)
    return;
  if (start_bit + count > highest_page)
    count = highest_page - start_bit;

  size_t idx = start_bit - lowest_page;
  size_t end_idx = idx + count;

  while (idx < end_idx && (idx % 8) != 0) {
    bitmap_clear(bm, idx);
    idx++;
  }

  if (end_idx > idx) {
    size_t full_bytes = (end_idx - idx) / 8;
    if (full_bytes > 0) {
      memset(&bm[idx / 8], 0, full_bytes);
      idx += full_bytes * 8;
    }
  }

  while (idx < end_idx) {
    bitmap_clear(bm, idx);
    idx++;
  }
}

static inline struct buddy_block *virt_to_buddy(uint64_t phys) {
  return (struct buddy_block *)(phys + physical_memory_offset);
}

static inline uint64_t buddy_to_phys(struct buddy_block *block) {
  return (uint64_t)block - physical_memory_offset;
}

static inline size_t get_order(size_t count) {
  size_t order = 0;
  size_t size = 1;
  while (size < count) {
    size *= 2;
    order++;
  }
  return order;
}

size_t pmm_get_free_pages(void) {
  size_t free_pages = 0;
  spinlock_acquire(&b_zone.lock);
  for (int order = 0; order < MAX_ORDER; order++) {
    struct list_head *pos;
    list_for_each(pos, &b_zone.free_list[order]) {
      free_pages += (1ULL << order);
    }
  }
  spinlock_release(&b_zone.lock);
  return free_pages;
}

static void buddy_free_internal(uint64_t phys, size_t order);

// Internal function to add a free block to the buddy system
static void buddy_free_internal(uint64_t phys, size_t order) {
  uint64_t pfn = phys / PAGE_SIZE;

  // Clear bitmap for this block
  bitmap_clear_range(bitmap, pfn, (1ULL << order));

  while (order < MAX_ORDER - 1) {
    uint64_t buddy_pfn = pfn ^ (1ULL << order);

    bool buddy_free = true;
    if (buddy_pfn < lowest_page || buddy_pfn >= highest_page ||
        !pmm_is_managed(buddy_pfn * PAGE_SIZE) ||
        bitmap_test(bitmap, buddy_pfn - lowest_page)) {
      buddy_free = false;
    }
    if (!buddy_free)
      break;

    struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
    if (buddy->order == order && buddy->node.next && buddy->node.prev) {
      // It's in the same order list, coalesce
      list_del(&buddy->node);
      pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
      order++;
    } else {
      // Free but split into smaller blocks; wait for them to coalesce up
      break;
    }
  }

  struct buddy_block *block = virt_to_buddy(pfn * PAGE_SIZE);
  block->order = order;
  list_add_tail(&block->node, &b_zone.free_list[order]);
}

void pmm_init_early(uint64_t hhdm_offset) {
  physical_memory_offset = hhdm_offset;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
  physical_memory_offset = hhdm_offset;
  internal_memmap = memmap;
  highest_page = 0;
  lowest_page = 0xFFFFFFFFFFFFFFFF;

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    if (entry->type == LIMINE_MEMMAP_USABLE ||
        entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
        entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES ||
        entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE ||
        entry->type == LIMINE_MEMMAP_ACPI_NVS ||
        entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
      total_memory += entry->length;
    }

    uint64_t top = entry->base + entry->length;

    // Only managed memory bounds determine metadata overhead
    if (entry->type == LIMINE_MEMMAP_USABLE ||
        entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
      if (entry->base / PAGE_SIZE < lowest_page) {
        lowest_page = entry->base / PAGE_SIZE;
      }
      if ((top + PAGE_SIZE - 1) / PAGE_SIZE > highest_page) {
        highest_page = (top + PAGE_SIZE - 1) / PAGE_SIZE;
      }
    }
  }

  if (highest_page > lowest_page) {
    page_count = highest_page - lowest_page;
  } else {
    page_count = 0;
  }

  bitmap_size = page_count / 8;
  if (page_count % 8 != 0)
    bitmap_size++;

  size_t refcount_table_size = page_count * sizeof(uint16_t);
  size_t total_metadata_size = bitmap_size * 2 + refcount_table_size;
  // Round up to avoid sharing a page between metadata and buddy-managed memory
  total_metadata_size = (total_metadata_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  klog_puts("[PMM] Required Metadata Size: ");
  klog_uint64(total_metadata_size / 1024);
  klog_puts(" KB\n");

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    if (entry->type == LIMINE_MEMMAP_USABLE &&
        entry->length >= total_metadata_size) {
      bitmap = (uint8_t *)(entry->base + hhdm_offset);
      managed_bitmap = bitmap + bitmap_size;
      refcounts = (uint16_t *)((uint64_t)managed_bitmap + bitmap_size);

      memset(bitmap, 0xFF, bitmap_size);
      memset(managed_bitmap, 0, bitmap_size);
      // We'll zero the refcount table per-range to avoid slow global memset on
      // sparse maps
      break;
    }
  }

  if (bitmap == NULL) {
    klog_puts("[PMM] FATAL: Could not find a large enough usable memory region "
              "for PMM metadata!\n");
    klog_puts("      Metadata Size: ");
    klog_uint64(total_metadata_size / 1024);
    klog_puts(" KB\n");
    klog_puts("      Max usable region seen: ");
    // Optional: add a log for the largest region seen to help debugging
    while (1)
      hal_cpu_halt();
  }

  for (int i = 0; i < MAX_ORDER; i++) {
    INIT_LIST_HEAD(&b_zone.free_list[i]);
  }

  uint64_t bitmap_phys_base = (uint64_t)bitmap - hhdm_offset;

  klog_puts("[PMM] Identified Physical Memory: ");
  klog_uint64(total_memory / 1024 / 1024);
  klog_puts(" MB\n");
  klog_puts("[PMM] Metadata overhead (Bitmaps + Refcounts): ");
  klog_uint64(total_metadata_size / 1024);
  klog_puts(" KB\n");

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    if (entry->type == LIMINE_MEMMAP_USABLE) {
      uint64_t entry_phys_base = entry->base;
      uint64_t entry_length = entry->length;

      uint64_t p = 0;
      while (p < entry_length) {
        uint64_t phys = entry_phys_base + p;
        uint64_t pfn = phys / PAGE_SIZE;

        // Skip first 1MB
        if (phys < 0x100000) {
          p += (0x100000 - phys);
          continue;
        }

        // Skip full metadata region (bitmap + managed_bitmap + refcounts)
        if (phys >= bitmap_phys_base &&
            phys < bitmap_phys_base + total_metadata_size) {
          p += (bitmap_phys_base + total_metadata_size - phys);
          continue;
        }

        // Determine the largest order we can use here.
        // It must be:
        // 1. Power of two pages
        // 2. Aligned to that power of two
        // 3. Not exceeding MAX_ORDER - 1
        // 4. Not overlapping with bitmap or 1MB reserve
        // 5. Fit within the remaining entry length

        size_t order = 0;
        while (order < MAX_ORDER - 1) {
          uint64_t next_order_pages = 1ULL << (order + 1);
          uint64_t next_order_size = next_order_pages * PAGE_SIZE;

          if (p + next_order_size > entry_length)
            break;
          if ((phys % next_order_size) != 0)
            break;

          // Check if next order would overlap with bitmap
          uint64_t phys_end = phys + next_order_size;
          if (!(phys_end <= bitmap_phys_base ||
                phys >= bitmap_phys_base + total_metadata_size)) {
            break;
          }

          order++;
        }

        // Mark as managed and free in one go
        bitmap_set_range(managed_bitmap, pfn, (1ULL << order));
        bitmap_clear_range(bitmap, pfn, (1ULL << order));

        // Sparse zeroing of the refcount table
        memset(&refcounts[pfn - lowest_page], 0,
               (1ULL << order) * sizeof(uint16_t));

        // Direct insert into Buddy free list (Boot optimization: avoid
        // coalescing logic)
        struct buddy_block *block = virt_to_buddy(phys);
        block->order = order;
        list_add_tail(&block->node, &b_zone.free_list[order]);

        usable_memory += (1ULL << order) * PAGE_SIZE;
        p += (1ULL << order) * PAGE_SIZE;
      }
    }
  }

  klog_puts("[PMM] Initialized. Usable memory: ");
  klog_uint64(usable_memory / 1024 / 1024);
  klog_puts(" MB\n");
}

void *pmm_alloc_pages(size_t count) {
  if (count == 0)
    return NULL;

  size_t order = get_order(count);
  if (order >= MAX_ORDER)
    return NULL;

  spinlock_acquire(&b_zone.lock);

  size_t cur_order = order;
  while (cur_order < MAX_ORDER && list_empty(&b_zone.free_list[cur_order])) {
    cur_order++;
  }

  if (cur_order == MAX_ORDER) {
    spinlock_release(&b_zone.lock);
    return NULL; // OOM
  }

  struct buddy_block *block =
      list_first_entry(&b_zone.free_list[cur_order], struct buddy_block, node);
  list_del(&block->node);

  uint64_t pfn = buddy_to_phys(block) / PAGE_SIZE;

  // Split down to requested order
  while (cur_order > order) {
    cur_order--;
    uint64_t buddy_pfn = pfn + (1ULL << cur_order);
    struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
    buddy->order = cur_order;
    list_add_tail(&buddy->node, &b_zone.free_list[cur_order]);
  }

  // Mark as used in bitmap
  bitmap_set_range(bitmap, pfn, 1ULL << order);

  // Initialize refcounts to 1 for the allocated pages
  for (size_t i = 0; i < (1ULL << order); i++) {
    refcounts[pfn + i - lowest_page] = 1;
  }

  spinlock_release(&b_zone.lock);
  void *res = (void *)(pfn * PAGE_SIZE);
  return res;
}

void *pmm_alloc_pages_constrained(size_t count, uint64_t max_phys_addr) {
  return pmm_alloc_pages_range(count, 0, max_phys_addr);
}

void *pmm_alloc_pages_range(size_t count, uint64_t min_phys_addr,
                            uint64_t max_phys_addr) {
  if (count == 0)
    return NULL;

  size_t order = get_order(count);
  if (order >= MAX_ORDER)
    return NULL;

  spinlock_acquire(&b_zone.lock);

  for (size_t cur_order = order; cur_order < MAX_ORDER; cur_order++) {
    struct buddy_block *found_block = NULL;
    uint64_t target_phys = 0;
    struct list_head *pos;

    list_for_each(pos, &b_zone.free_list[cur_order]) {
      struct buddy_block *block = list_entry(pos, struct buddy_block, node);
      uint64_t phys = buddy_to_phys(block);
      uint64_t block_end = phys + (1ULL << cur_order) * PAGE_SIZE;
      uint64_t allocation_size = (1ULL << order) * PAGE_SIZE;
      uint64_t candidate = phys > min_phys_addr ? phys : min_phys_addr;
      candidate = (candidate + allocation_size - 1) & ~(allocation_size - 1);
      uint64_t allowed_end = block_end < max_phys_addr ? block_end
                                                       : max_phys_addr;
      /* A larger buddy may straddle the requested DMA boundary. It is still
       * usable when one of its descendants lies wholly inside the range. */
      if (candidate < allowed_end &&
          allocation_size <= allowed_end - candidate) {
        found_block = block;
        target_phys = candidate;
        break;
      }
    }

    if (found_block) {
      list_del(&found_block->node);
      uint64_t pfn = buddy_to_phys(found_block) / PAGE_SIZE;
      uint64_t target_pfn = target_phys / PAGE_SIZE;

      /* Split toward the selected descendant, returning the unused sibling at
       * every level to its corresponding free list. */
      while (cur_order > order) {
        cur_order--;
        uint64_t half_pages = 1ULL << cur_order;
        uint64_t buddy_pfn;
        if (target_pfn >= pfn + half_pages) {
          buddy_pfn = pfn;
          pfn += half_pages;
        } else {
          buddy_pfn = pfn + half_pages;
        }
        struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
        buddy->order = cur_order;
        list_add_tail(&buddy->node, &b_zone.free_list[cur_order]);
      }

      // Mark as used in bitmap
      bitmap_set_range(bitmap, pfn, 1ULL << order);
      for (size_t i = 0; i < (1ULL << order); i++)
        refcounts[pfn + i - lowest_page] = 1;

      spinlock_release(&b_zone.lock);
      return (void *)(pfn * PAGE_SIZE);
    }
  }

  spinlock_release(&b_zone.lock);
  return NULL; // No block found within constraints
}

void *pmm_alloc_page(void) { return pmm_alloc_pages(1); }

void pmm_free_pages(void *ptr, size_t count) {
  if (!ptr || count == 0)
    return;

  size_t order = get_order(count);
  if (order >= MAX_ORDER)
    return;

  uint64_t addr = (uint64_t)ptr;
  uint64_t pfn = addr / PAGE_SIZE;

  if (pfn < lowest_page || pfn >= highest_page || !bitmap_test(managed_bitmap, pfn - lowest_page)) {
    return; // Not managed by buddy allocator (e.g. MMIO, framebuffer)
  }

  if (!bitmap_test(bitmap, pfn - lowest_page)) {
    return; // Already free (prevents double-free list corruption)
  }

  spinlock_acquire(&b_zone.lock);

  // Check refcounts for the range. We only free pages whose refcount hits zero.
  // NOTE: pmm_free_pages with count > 1 is currently only used for
  // non-refcounted internal kernel allocations (like reclaiming bootloader
  // memory). For user pages (CoW), we always use pmm_decref (which calls
  // pmm_free_page).
  for (size_t i = 0; i < (1ULL << order); i++) {
    if (refcounts[pfn + i - lowest_page] > 0) {
      refcounts[pfn + i - lowest_page]--;
    }
  }

  if (refcounts[pfn - lowest_page] == 0) {
    buddy_free_internal(addr, order);
  }

  spinlock_release(&b_zone.lock);
}

void pmm_free_page(void *ptr) { pmm_decref(ptr); }

bool pmm_is_managed(uint64_t phys) {
  uint64_t pfn = phys / PAGE_SIZE;
  if (pfn < lowest_page || pfn >= highest_page || managed_bitmap == NULL)
    return false;
  return bitmap_test(managed_bitmap, pfn - lowest_page);
}

void pmm_incref(void *ptr) {
  if (!ptr)
    return;
  if (!pmm_is_managed((uint64_t)ptr))
    return;
  uint64_t pfn = (uint64_t)ptr / PAGE_SIZE;

  spinlock_acquire(&pmm_lock);
  refcounts[pfn - lowest_page]++;
  spinlock_release(&pmm_lock);
}

void pmm_decref(void *ptr) {
  if (!ptr)
    return;
  if (!pmm_is_managed((uint64_t)ptr))
    return;
  uint64_t pfn = (uint64_t)ptr / PAGE_SIZE;

  spinlock_acquire(&pmm_lock);
  if (refcounts[pfn - lowest_page] > 0) {
    refcounts[pfn - lowest_page]--;
    if (refcounts[pfn - lowest_page] == 0) {
      spinlock_release(&pmm_lock);

      spinlock_acquire(&b_zone.lock);
      buddy_free_internal((uint64_t)ptr, 0);
      spinlock_release(&b_zone.lock);
      return;
    }
  }
  spinlock_release(&pmm_lock);
}

uint16_t pmm_get_ref(void *ptr) {
  if (!ptr)
    return 0;
  if (!pmm_is_managed((uint64_t)ptr))
    return 1; // Hardware is always "referenced"
  uint64_t pfn = (uint64_t)ptr / PAGE_SIZE;
  return refcounts[pfn - lowest_page];
}

void pmm_mark_used(void *ptr, size_t count) {
  if (!ptr || count == 0)
    return;
  uint64_t addr = (uint64_t)ptr;
  size_t start_bit = addr / PAGE_SIZE;
  bitmap_set_range(bitmap, start_bit, count);
}

bool pmm_is_reclaimable(uint64_t phys) {
  if (!internal_memmap)
    return false;
  for (uint64_t i = 0; i < internal_memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = internal_memmap->entries[i];
    if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
      if (phys >= entry->base && phys < (entry->base + entry->length)) {
        return true;
      }
    }
  }
  return false;
}

void pmm_reclaim_bootloader(uint64_t kernel_phys_base) {
  if (!internal_memmap)
    return;

  // STEP 0: Buffer the memory map locally.
  // The original memmap structure is in Bootloader Reclaimable memory!
  // If we start freeing it while looping over it, we will crash.
  static struct limine_memmap_entry static_entries[128];
  uint64_t entry_count = internal_memmap->entry_count;
  if (entry_count > 128)
    entry_count = 128;

  for (uint64_t i = 0; i < entry_count; i++) {
    static_entries[i] = *internal_memmap->entries[i];
  }

  klog_puts("[PMM] Analyzing buffered memory map for reclamation...\n");

  extern void vmm_protect_active_tables(void);

  // Allocate a temporary bitmap to track which pages are reclaimable.
  size_t temp_count = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint8_t *temp_bitmap_phys = pmm_alloc_pages(temp_count);
  if (!temp_bitmap_phys) {
    klog_puts("[PMM] ERROR: Failed to allocate temp bitmap for reclaim.\n");
    return;
  }
  uint8_t *temp_bitmap =
      (uint8_t *)((uint64_t)temp_bitmap_phys + physical_memory_offset);
  memset(temp_bitmap, 0, bitmap_size);

  // Step 1: Mark all reclaimable regions (excluding ACPI until fully parsed)
  for (uint64_t i = 0; i < entry_count; i++) {
    struct limine_memmap_entry *entry = &static_entries[i];

    bool reclaim = false;
    // We EXCLUDE ACPI_RECLAIMABLE for now as we might need SDTs later (e.g.
    // MCFG discovery)
    if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
      reclaim = true;
    } else if (entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
      // Keep executable/module pages reserved. Boot modules may directly back
      // long-lived devices such as ram0, so reclaiming them would corrupt the
      // device after registration.
      (void)kernel_phys_base;
    }

    if (reclaim) {
      uint64_t start = entry->base;
      uint64_t len = entry->length;
      if (start < 0x100000) {
        uint64_t diff = 0x100000 - start;
        if (diff >= len)
          continue;
        start = 0x100000;
        len -= diff;
      }
      size_t pfn_start = start / PAGE_SIZE;
      size_t pfn_count = len / PAGE_SIZE;
      bitmap_set_range(temp_bitmap, pfn_start, pfn_count);
      // DON'T clear main bitmap yet; wait until we ensure VMM doesn't need them
    }
  }

  // Step 2: VMM marks active page tables as used in main bitmap
  vmm_protect_active_tables();

  // Step 3: Safety check: If a page is marked for reclaim but VMM says it's
  // used, don't reclaim.
  for (size_t b = 0; b < bitmap_size; b++) {
    if (temp_bitmap[b]) {
      for (int i = 0; i < 8; i++) {
        if (temp_bitmap[b] & (1 << i)) {
          uint64_t pfn = b * 8 + i + lowest_page;
          if (bitmap_test(bitmap, pfn - lowest_page)) {
            // Locked by VMM, remove from reclaim set
            temp_bitmap[b] &= ~(1 << i);
          }
        }
      }
    }
  }

  // Step 4: Safely perform the reclamation
  klog_puts("[PMM] Reclaiming unprotected bootloader pages...\n");

  // Disable interrupts during final stage to avoid scheduler/allocation races
  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&b_zone.lock);

  for (size_t b = 0; b < bitmap_size; b++) {
    if (temp_bitmap[b]) {
      for (int i = 0; i < 8; i++) {
        if (temp_bitmap[b] & (1 << i)) {
          uint64_t start_pfn = (uint64_t)b * 8 + i + lowest_page;
          size_t count = 0;
          uint64_t curr_pfn = start_pfn;

          while (curr_pfn < highest_page) {
            size_t curr_idx = curr_pfn - lowest_page;
            if (temp_bitmap[curr_idx / 8] & (1 << (curr_idx % 8))) {
              count++;
              temp_bitmap[curr_idx / 8] &= ~(1 << (curr_idx % 8));
              curr_pfn++;
            } else
              break;
          }

          uint64_t p = 0;
          while (p < count) {
            uint64_t phys = (start_pfn + p) * PAGE_SIZE;
            size_t remaining = count - p;
            size_t order = 0;
            while (order < MAX_ORDER - 1) {
              uint64_t n_pages = 1ULL << (order + 1);
              if (n_pages > remaining || (phys % (n_pages * PAGE_SIZE)) != 0)
                break;
              order++;
            }

            uint64_t p_count = 1ULL << order;
            uint64_t base_pfn = phys / PAGE_SIZE;
            bitmap_set_range(managed_bitmap, base_pfn, p_count);
            memset(&refcounts[base_pfn - lowest_page], 0,
                   p_count * sizeof(uint16_t));
            buddy_free_internal(phys, order);
            usable_memory += p_count * PAGE_SIZE;
            p += p_count;
          }
        }
      }
    }
  }

  internal_memmap = NULL;
  spinlock_release(&b_zone.lock);
  hal_irq_restore(flags);

  klog_puts("[PMM] Bootloader memory reclaimed. Usable RAM now: ");
  klog_uint64(usable_memory / 1024 / 1024);
  klog_puts(" MB\n");

  pmm_free_pages(temp_bitmap_phys, temp_count);
}

uint64_t pmm_get_usable_memory(void) { return usable_memory; }
uint64_t pmm_get_total_memory(void) { return total_memory; }
uint64_t pmm_get_hhdm_offset(void) { return physical_memory_offset; }
