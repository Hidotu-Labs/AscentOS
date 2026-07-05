#include "mm/heap.h"
#include "console/console.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define SLAB_MAGIC 0x51ABCAFECAFE51ABULL
#define BIG_MAGIC 0xB16A110CB16A110CULL

#define BITMAP_SET(bmp, index) ((bmp)[(index) / 32] |= (1 << ((index) % 32)))
#define BITMAP_CLEAR(bmp, index) ((bmp)[(index) / 32] &= ~(1 << ((index) % 32)))
#define BITMAP_TEST(bmp, index)                                                \
  (((bmp)[(index) / 32] & (1 << ((index) % 32))) != 0)

static spinlock_t heap_lock = SPINLOCK_INIT;
static uint64_t current_heap_vaddr = KERNEL_HEAP_BASE;
bool heap_initialized = false;

#define HEAP_FREE_EXTENTS 256
struct heap_free_extent {
  uint64_t vaddr;
  size_t pages;
};
static struct heap_free_extent free_extents[HEAP_FREE_EXTENTS];

struct slab_cache;

struct slab {
  uint64_t magic;
  struct slab *next;
  struct slab *prev;
  uint32_t free_count;
  uint32_t total_count;
  struct slab_cache *cache;
  uint32_t bitmap[4]; // 128 bits
} __attribute__((aligned(64)));

struct slab_cache {
  size_t obj_size;
  struct slab *partial;
  struct slab *full;
  struct slab *free;
};

struct big_alloc {
  uint64_t magic;
  size_t pages;
  struct big_alloc *next;
  struct big_alloc *prev;
  uint64_t padding[4];
} __attribute__((aligned(64)));

static struct big_alloc *big_alloc_head = NULL;

static struct slab_cache caches[] = {
    {32, NULL, NULL, NULL},  {64, NULL, NULL, NULL},  {128, NULL, NULL, NULL},
    {256, NULL, NULL, NULL}, {512, NULL, NULL, NULL}, {1024, NULL, NULL, NULL}};
#define CACHE_COUNT (sizeof(caches) / sizeof(caches[0]))

// Virtual address space bumper
static uint64_t allocate_virtual_space(size_t pages) {
  for (size_t i = 0; i < HEAP_FREE_EXTENTS; i++) {
    if (free_extents[i].pages < pages)
      continue;
    uint64_t vaddr = free_extents[i].vaddr;
    free_extents[i].vaddr += pages * PAGE_SIZE;
    free_extents[i].pages -= pages;
    if (free_extents[i].pages == 0)
      free_extents[i].vaddr = 0;
    return vaddr;
  }

  uint64_t vaddr = current_heap_vaddr;
  current_heap_vaddr += pages * PAGE_SIZE;
  return vaddr;
}

static void release_virtual_space(uint64_t vaddr, size_t pages) {
  if (!vaddr || !pages)
    return;

  if (vaddr + pages * PAGE_SIZE == current_heap_vaddr) {
    current_heap_vaddr = vaddr;
    bool merged;
    do {
      merged = false;
      for (size_t i = 0; i < HEAP_FREE_EXTENTS; i++) {
        if (free_extents[i].pages &&
            free_extents[i].vaddr + free_extents[i].pages * PAGE_SIZE ==
                current_heap_vaddr) {
          current_heap_vaddr = free_extents[i].vaddr;
          free_extents[i].vaddr = 0;
          free_extents[i].pages = 0;
          merged = true;
          break;
        }
      }
    } while (merged);
    return;
  }

  for (size_t i = 0; i < HEAP_FREE_EXTENTS; i++) {
    if (!free_extents[i].pages)
      continue;
    if (free_extents[i].vaddr + free_extents[i].pages * PAGE_SIZE == vaddr) {
      free_extents[i].pages += pages;
      return;
    }
    if (vaddr + pages * PAGE_SIZE == free_extents[i].vaddr) {
      free_extents[i].vaddr = vaddr;
      free_extents[i].pages += pages;
      return;
    }
  }

  for (size_t i = 0; i < HEAP_FREE_EXTENTS; i++) {
    if (free_extents[i].pages == 0) {
      free_extents[i].vaddr = vaddr;
      free_extents[i].pages = pages;
      return;
    }
  }
}

void heap_init(void) {
  heap_initialized = true;
  // Rely on static zeroes and VMM/PMM already being up
}

static struct slab *allocate_new_slab(struct slab_cache *c) {
  void *frame = pmm_alloc_page();
  if (!frame)
    return NULL;

  uint64_t vaddr = allocate_virtual_space(1);
  uint64_t *pml4 = vmm_get_active_pml4();

  if (!vmm_map_page(pml4, vaddr, (uint64_t)frame,
                    PAGE_FLAG_PRESENT | PAGE_FLAG_RW)) {
    pmm_free_page(frame);
    return NULL;
  }

  struct slab *s = (struct slab *)vaddr;
  memset(s, 0, PAGE_SIZE);

  s->magic = SLAB_MAGIC;
  s->cache = c;
  s->total_count = (PAGE_SIZE - sizeof(struct slab)) / c->obj_size;
  if (s->total_count > 128)
    s->total_count = 128; // Limit by bitmap capacity
  s->free_count = s->total_count;

  // By default, a newly allocated and prepared block is considered "free
  // array", so we set bitmap appropriately Wait, the bitmap represents objects
  // that are ALLOCATED, so zero means free. Thus memset 0 handles it.

  return s;
}

void *kmalloc(size_t size) {
  if (size == 0)
    return NULL;

  spinlock_acquire(&heap_lock);

  // 1. Can we fit it in a slab cache?
  struct slab_cache *c = NULL;
  for (size_t i = 0; i < CACHE_COUNT; i++) {
    if (size <= caches[i].obj_size) {
      c = &caches[i];
      break;
    }
  }

  if (c) {
    // Slab Allocation Path
    struct slab *s = c->partial;

    // If no partial, try to get from free list, else allocate new page
    if (!s) {
      if (c->free) {
        s = c->free;
        c->free = s->next;
        if (c->free)
          c->free->prev = NULL;
        s->next = NULL;
      } else {
        s = allocate_new_slab(c);
        if (!s) {
          spinlock_release(&heap_lock);
          return NULL;
        }
      }
      // Put it in partial list
      s->next = c->partial;
      s->prev = NULL;
      if (c->partial)
        c->partial->prev = s;
      c->partial = s;
    }

    // Find free index in partial slab
    int free_idx = -1;
    for (int i = 0; i < (int)s->total_count; i++) {
      if (!BITMAP_TEST(s->bitmap, i)) {
        free_idx = i;
        break;
      }
    }

    if (free_idx == -1) {
      // Should theoretically never happen as partial holds slabs with free
      // slots.
      spinlock_release(&heap_lock);
      return NULL;
    }

    BITMAP_SET(s->bitmap, free_idx);
    s->free_count--;

    // If slab is now full, move it from partial to full list
    if (s->free_count == 0) {
      if (s->prev)
        s->prev->next = s->next;
      else
        c->partial = s->next;
      if (s->next)
        s->next->prev = s->prev;

      s->next = c->full;
      s->prev = NULL;
      if (c->full)
        c->full->prev = s;
      c->full = s;
    }

    uint8_t *obj_base = (uint8_t *)s + sizeof(struct slab);
    void *ptr = obj_base + (free_idx * c->obj_size);

    spinlock_release(&heap_lock);
    return ptr;
  }

  // 2. Large Allocation Path (> 1024 bytes)
  size_t total_size = size + sizeof(struct big_alloc);
  size_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

  void *blocks = pmm_alloc_pages(pages);
  if (!blocks) {
    spinlock_release(&heap_lock);
    return NULL;
  }

  uint64_t vaddr = allocate_virtual_space(pages);
  uint64_t *pml4 = vmm_get_active_pml4();
  if (!vmm_map_range(pml4, vaddr, (uint64_t)blocks, pages,
                     PAGE_FLAG_PRESENT | PAGE_FLAG_RW)) {
    pmm_free_pages(blocks, pages);
    spinlock_release(&heap_lock);
    return NULL;
  }

  struct big_alloc *b = (struct big_alloc *)vaddr;
  b->magic = BIG_MAGIC;
  b->pages = pages;

  // Link globally
  b->next = big_alloc_head;
  b->prev = NULL;
  if (big_alloc_head)
    big_alloc_head->prev = b;
  big_alloc_head = b;

  void *ptr = (void *)((uint8_t *)b + sizeof(struct big_alloc));
  spinlock_release(&heap_lock);
  return ptr;
}

void kfree(void *ptr) {
  if (!ptr)
    return;

  spinlock_acquire(&heap_lock);

  uint64_t page_base = (uint64_t)ptr & ~0xFFFULL;
  uint64_t magic_check = *(uint64_t *)page_base;

  // Is it a Slab chunk?
  if (magic_check == SLAB_MAGIC) {
    struct slab *s = (struct slab *)page_base;
    struct slab_cache *c = s->cache;

    uint64_t offset = (uint64_t)ptr - (page_base + sizeof(struct slab));
    uint32_t idx = offset / c->obj_size;

    if (!BITMAP_TEST(s->bitmap, idx)) {
      console_puts("[WARN] kfree: Double free intercepted inside Slab!\n");
      spinlock_release(&heap_lock);
      return;
    }

    BITMAP_CLEAR(s->bitmap, idx);
    s->free_count++;

    // State machine updates
    // If it was full, it is now partial
    if (s->free_count == 1) {
      // Remove from full
      if (s->prev)
        s->prev->next = s->next;
      else
        c->full = s->next;
      if (s->next)
        s->next->prev = s->prev;

      // Link to partial
      s->next = c->partial;
      s->prev = NULL;
      if (c->partial)
        c->partial->prev = s;
      c->partial = s;
    }

    // If it's completely empty, move to the free list for later reuse.
    // We do NOT unmap/free the underlying page because kernel heap
    // page tables are shallow-copied (shared) across all process PML4s.
    // Calling vmm_unmap_page → vmm_free_empty_tables would destroy
    // shared intermediate PT/PD/PDPT pages, corrupting other processes'
    // page table walks and causing kfree magic validation failures.
    if (s->free_count == s->total_count) {
      // Remove from partial list
      if (s->prev)
        s->prev->next = s->next;
      else
        c->partial = s->next;
      if (s->next)
        s->next->prev = s->prev;

      // Move to the cache's free list for reuse
      s->next = c->free;
      s->prev = NULL;
      if (c->free)
        c->free->prev = s;
      c->free = s;
    }

    spinlock_release(&heap_lock);
    return;
  }

  // Is it a Big Alloc chunk?
  if (magic_check == BIG_MAGIC) {
    struct big_alloc *b = (struct big_alloc *)page_base;

    // Unlink globally
    if (b->prev)
      b->prev->next = b->next;
    else
      big_alloc_head = b->next;
    if (b->next)
      b->next->prev = b->prev;

    size_t pages = b->pages;
    uint64_t *pml4 = vmm_get_active_pml4();

    uint64_t phys_addr = vmm_virt_to_phys(pml4, page_base);

    for (size_t i = 0; i < pages; i++) {
      vmm_unmap_page(pml4, page_base + i * PAGE_SIZE);
    }

    pmm_free_pages((void *)phys_addr, pages);
    release_virtual_space(page_base, pages);

    spinlock_release(&heap_lock);
    return;
  }

  console_puts(
      "[WARN] kfree: Fatal validation failure. Invalid pointer space.\n");
  klog_puts("  ptr=");
  klog_uint64((uint64_t)ptr);
  klog_puts(" page_base=");
  klog_uint64(page_base);
  klog_puts(" magic=");
  klog_uint64(magic_check);
  klog_puts("\n");
  spinlock_release(&heap_lock);
}

void *kcalloc(size_t num, size_t size) {
  size_t total = num * size;
  void *ptr = kmalloc(total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
  if (!ptr)
    return kmalloc(new_size);
  if (new_size == 0) {
    kfree(ptr);
    return NULL;
  }

  // To determine old size safely without deadlocking, acquire spinlock briefly
  // inside lookup
  spinlock_acquire(&heap_lock);
  uint64_t page_base = (uint64_t)ptr & ~0xFFFULL;
  uint64_t magic_check = *(uint64_t *)page_base;
  size_t old_size = 0;

  if (magic_check == SLAB_MAGIC) {
    struct slab *s = (struct slab *)page_base;
    old_size = s->cache->obj_size;
  } else if (magic_check == BIG_MAGIC) {
    struct big_alloc *b = (struct big_alloc *)page_base;
    old_size = (b->pages * PAGE_SIZE) - sizeof(struct big_alloc);
  } else {
    spinlock_release(&heap_lock);
    return NULL;
  }
  spinlock_release(&heap_lock);

  if (new_size <= old_size)
    return ptr; // Abort reallocation if size is sufficient

  void *new_ptr = kmalloc(new_size);
  if (new_ptr) {
    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
  }
  return new_ptr;
}

static void heap_u64_to_str(uint64_t val, char *buf) {
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char temp[32];
  int i = 0;
  while (val > 0) {
    temp[i++] = (val % 10) + '0';
    val /= 10;
  }
  int j = 0;
  while (i > 0) {
    buf[j++] = temp[--i];
  }
  buf[j] = '\0';
}

void heap_get_info(char *buf) {
  spinlock_acquire(&heap_lock);
  buf[0] = '\0';
  char num_buf[32];

  strcat(buf, "Slab Statistics:\n");
  strcat(buf, "  Size | Total Slabs | Total Objs | Free Objs\n");
  strcat(buf, "-------|-------------|------------|-----------\n");

  for (size_t i = 0; i < CACHE_COUNT; i++) {
    struct slab_cache *c = &caches[i];
    uint32_t total_slabs = 0;
    uint32_t total_objs = 0;
    uint32_t free_objs = 0;

    // Count in partial list
    for (struct slab *s = c->partial; s; s = s->next) {
      total_slabs++;
      total_objs += s->total_count;
      free_objs += s->free_count;
    }

    // Count in full list
    for (struct slab *s = c->full; s; s = s->next) {
      total_slabs++;
      total_objs += s->total_count;
      free_objs += s->free_count;
    }

    // Count in free list
    for (struct slab *s = c->free; s; s = s->next) {
      total_slabs++;
      total_objs += s->total_count;
      free_objs += s->free_count;
    }

    strcat(buf, "  ");
    heap_u64_to_str(c->obj_size, num_buf);
    strcat(buf, num_buf);
    int pad = 5 - strlen(num_buf);
    while (pad-- > 0)
      strcat(buf, " ");
    strcat(buf, "| ");

    heap_u64_to_str(total_slabs, num_buf);
    strcat(buf, num_buf);
    pad = 12 - strlen(num_buf);
    while (pad-- > 0)
      strcat(buf, " ");
    strcat(buf, "| ");

    heap_u64_to_str(total_objs, num_buf);
    strcat(buf, num_buf);
    pad = 11 - strlen(num_buf);
    while (pad-- > 0)
      strcat(buf, " ");
    strcat(buf, "| ");

    heap_u64_to_str(free_objs, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");
  }

  strcat(buf, "\nBig Allocations:\n");
  uint32_t big_count = 0;
  uint64_t big_pages = 0;
  for (struct big_alloc *b = big_alloc_head; b; b = b->next) {
    big_count++;
    big_pages += b->pages;
  }

  strcat(buf, "  Count: ");
  heap_u64_to_str(big_count, num_buf);
  strcat(buf, num_buf);
  strcat(buf, "\n");

  strcat(buf, "  Total Pages: ");
  heap_u64_to_str(big_pages, num_buf);
  strcat(buf, num_buf);
  strcat(buf, "\n");

  strcat(buf, "  Total Size: ");
  heap_u64_to_str(big_pages * PAGE_SIZE / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");

  spinlock_release(&heap_lock);
}
