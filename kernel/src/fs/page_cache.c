#include "vfs.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../sched/sched.h"

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))
#define CACHE_BATCH 64u
#define CACHE_NODE_LIMIT 256u

static uint64_t vfs_cached_pages;
static uint64_t vfs_cache_clock;

static uint64_t cache_stamp(void) {
  return __atomic_add_fetch(&vfs_cache_clock, 1, __ATOMIC_RELAXED);
}

static uint64_t cache_key(uint32_t offset) { return (uint64_t)(offset >> 12); }

size_t vfs_cache_page_count(void) {
  return (size_t)__atomic_load_n(&vfs_cached_pages, __ATOMIC_RELAXED);
}

static void cache_page_release(vfs_page_t *page) {
  pmm_free_page((void *)page->frame_phys);
  kfree(page);
}

vfs_page_t *vfs_cache_lookup(vfs_node_t *node, uint32_t offset) {
  if (!node)
    return NULL;
  spinlock_acquire(&node->pages_lock);
  vfs_page_t *page = radix_tree_lookup(&node->pages, cache_key(offset));
  if (page) {
    page->refs++;
    page->last_used = cache_stamp();
  }
  spinlock_release(&node->pages_lock);
  return page;
}

void vfs_cache_put(vfs_node_t *node, vfs_page_t *page) {
  if (!node || !page)
    return;
  bool release = false;
  spinlock_acquire(&node->pages_lock);
  if (page->refs)
    page->refs--;
  if (!page->refs && page->evicted)
    release = true;
  spinlock_release(&node->pages_lock);
  if (release)
    cache_page_release(page);
}

vfs_page_t *vfs_cache_insert(vfs_node_t *node, uint32_t offset,
                             uint64_t frame) {
  if (!node)
    return NULL;
  offset &= ~(PAGE_SIZE - 1);
  vfs_page_t *new_page = kmalloc(sizeof(*new_page));
  if (!new_page)
    return NULL;
  memset(new_page, 0, sizeof(*new_page));
  new_page->offset = offset;
  new_page->frame_phys = frame;
  new_page->uptodate = true;
  new_page->refs = 1;
  new_page->last_used = cache_stamp();

  spinlock_acquire(&node->pages_lock);
  vfs_page_t *page = radix_tree_lookup(&node->pages, cache_key(offset));
  if (page) {
    page->refs++;
    spinlock_release(&node->pages_lock);
    kfree(new_page);
    pmm_free_page((void *)frame);
    return page;
  }
  if (radix_tree_insert(&node->pages, cache_key(offset), new_page)) {
    spinlock_release(&node->pages_lock);
    kfree(new_page);
    return NULL;
  }
  __atomic_add_fetch(&vfs_cached_pages, 1, __ATOMIC_RELAXED);
  spinlock_release(&node->pages_lock);
  return new_page;
}

vfs_page_t *vfs_cache_get_or_create(vfs_node_t *node, uint32_t offset) {
  vfs_page_t *page = vfs_cache_lookup(node, offset);
  if (page) {
    while (1) {
      spinlock_acquire(&node->pages_lock);
      bool loading = page->loading;
      bool uptodate = page->uptodate;
      spinlock_release(&node->pages_lock);
      if (!loading) {
        if (uptodate)
          return page;
        vfs_cache_put(node, page);
        return NULL;
      }
      sched_yield();
    }
  }
  void *frame = pmm_alloc_page();
  if (!frame)
    return NULL;

  offset &= ~(PAGE_SIZE - 1);
  vfs_page_t *candidate = kmalloc(sizeof(*candidate));
  if (!candidate) {
    pmm_free_page(frame);
    return NULL;
  }
  memset(candidate, 0, sizeof(*candidate));
  candidate->offset = offset;
  candidate->frame_phys = (uint64_t)frame;
  candidate->loading = true;
  candidate->refs = 1;
  candidate->last_used = cache_stamp();

  bool creator = false;
  spinlock_acquire(&node->pages_lock);
  page = radix_tree_lookup(&node->pages, cache_key(offset));
  if (page) {
    page->refs++;
  } else if (!radix_tree_insert(&node->pages, cache_key(offset), candidate)) {
    page = candidate;
    creator = true;
    __atomic_add_fetch(&vfs_cached_pages, 1, __ATOMIC_RELAXED);
  }
  spinlock_release(&node->pages_lock);

  if (!creator) {
    kfree(candidate);
    pmm_free_page(frame);
    if (!page)
      return NULL;
    while (1) {
      spinlock_acquire(&node->pages_lock);
      bool loading = page->loading;
      bool uptodate = page->uptodate;
      spinlock_release(&node->pages_lock);
      if (!loading) {
        if (uptodate)
          return page;
        vfs_cache_put(node, page);
        return NULL;
      }
      sched_yield();
    }
  }

  uint32_t available = node->length > offset ? node->length - offset : 0;
  uint32_t to_read = available > PAGE_SIZE ? PAGE_SIZE : available;
  uint32_t read = 0;
  if (to_read && node->read)
    read = node->read(node, offset, to_read, PHYS_TO_VIRT((uint64_t)frame));
  if (read < PAGE_SIZE)
    memset((uint8_t *)PHYS_TO_VIRT((uint64_t)frame) + read, 0,
           PAGE_SIZE - read);

  spinlock_acquire(&node->pages_lock);
  page->uptodate = read == to_read && !page->evicted;
  page->loading = false;
  bool ok = page->uptodate;
  spinlock_release(&node->pages_lock);
  if (!ok) {
    vfs_cache_invalidate(node, offset);
    vfs_cache_put(node, page);
    return NULL;
  }
  return page;
}

uint32_t vfs_cache_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer) {
  if (!node || !buffer || !size || offset >= node->length)
    return 0;
  if (size > node->length - offset)
    size = node->length - offset;

  uint32_t done = 0;
  while (done < size) {
    uint32_t current = offset + done;
    uint32_t page_offset = current & ~(PAGE_SIZE - 1);
    uint32_t in_page = current & (PAGE_SIZE - 1);
    uint32_t count = PAGE_SIZE - in_page;
    if (count > size - done)
      count = size - done;
    vfs_page_t *page = vfs_cache_get_or_create(node, page_offset);
    if (!page)
      break;
    memcpy(buffer + done,
           (uint8_t *)PHYS_TO_VIRT(page->frame_phys) + in_page, count);
    vfs_cache_put(node, page);
    done += count;
  }
  return done;
}

static vfs_page_t *cache_delete_locked(vfs_node_t *node, uint64_t key) {
  vfs_page_t *page = radix_tree_delete(&node->pages, key);
  if (!page)
    return NULL;
  page->evicted = true;
  __atomic_sub_fetch(&vfs_cached_pages, 1, __ATOMIC_RELAXED);
  return page;
}

void vfs_cache_invalidate(vfs_node_t *node, uint32_t offset) {
  if (!node)
    return;
  spinlock_acquire(&node->pages_lock);
  vfs_page_t *page = cache_delete_locked(node, cache_key(offset));
  bool release = page && !page->refs;
  spinlock_release(&node->pages_lock);
  if (release)
    cache_page_release(page);
}

struct key_batch {
  uint64_t keys[CACHE_BATCH];
  uint32_t count;
  uint64_t first;
  uint64_t last;
  bool unused_only;
};

static bool collect_keys(uint64_t key, void *value, void *opaque) {
  struct key_batch *batch = opaque;
  vfs_page_t *page = value;
  if (key < batch->first || key > batch->last)
    return true;
  if (batch->unused_only &&
      (page->refs || page->dirty || page->loading || page->writeback ||
       pmm_get_ref((void *)page->frame_phys) != 1))
    return true;
  batch->keys[batch->count++] = key;
  return batch->count < CACHE_BATCH;
}

static void cache_remove_range(vfs_node_t *node, uint64_t first, uint64_t last,
                               bool unused_only) {
  while (1) {
    struct key_batch batch = {
        .first = first, .last = last, .unused_only = unused_only};
    bool complete = radix_tree_for_each_range(&node->pages, first, last,
                                               collect_keys, &batch);
    if (!batch.count)
      break;
    for (uint32_t i = 0; i < batch.count; i++) {
      vfs_page_t *page = cache_delete_locked(node, batch.keys[i]);
      if (!page)
        continue;
      if (!page->refs)
        cache_page_release(page);
    }
    if (complete)
      break;
  }
}

void vfs_cache_invalidate_range(vfs_node_t *node, uint32_t offset,
                                uint32_t length) {
  if (!node || !length)
    return;
  uint64_t first = cache_key(offset);
  uint64_t end = (uint64_t)offset + length - 1;
  uint64_t last = end >> 12;
  spinlock_acquire(&node->pages_lock);
  cache_remove_range(node, first, last, false);
  spinlock_release(&node->pages_lock);
}

static void cache_destroy_value(void *value) {
  vfs_page_t *page = value;
  page->evicted = true;
  __atomic_sub_fetch(&vfs_cached_pages, 1, __ATOMIC_RELAXED);
  if (!page->refs)
    cache_page_release(page);
}

void vfs_cache_clear(vfs_node_t *node) {
  if (!node)
    return;
  spinlock_acquire(&node->pages_lock);
  radix_tree_destroy(&node->pages, cache_destroy_value);
  spinlock_release(&node->pages_lock);
}

void vfs_cache_clear_unused(vfs_node_t *node) {
  if (!node)
    return;
  spinlock_acquire(&node->pages_lock);
  cache_remove_range(node, 0, UINT64_MAX, true);
  spinlock_release(&node->pages_lock);
}

struct reclaim_search {
  uint64_t key;
  uint64_t stamp;
  bool found;
};

static bool find_reclaimable(uint64_t key, void *value, void *opaque) {
  vfs_page_t *page = value;
  struct reclaim_search *search = opaque;
  if (page->refs || page->dirty || page->loading || page->writeback ||
      pmm_get_ref((void *)page->frame_phys) != 1)
    return true;
  if (!search->found || page->last_used < search->stamp) {
    search->found = true;
    search->key = key;
    search->stamp = page->last_used;
  }
  return true;
}

size_t vfs_cache_reclaim(vfs_node_t *node, size_t target) {
  if (!node || !target)
    return 0;
  size_t reclaimed = 0;
  spinlock_acquire(&node->pages_lock);
  while (reclaimed < target) {
    struct reclaim_search search = {0};
    radix_tree_for_each(&node->pages, find_reclaimable, &search);
    if (!search.found)
      break;
    vfs_page_t *page = cache_delete_locked(node, search.key);
    if (!page)
      continue;
    cache_page_release(page);
    reclaimed++;
  }
  spinlock_release(&node->pages_lock);
  return reclaimed;
}

void vfs_cache_mark_dirty(vfs_node_t *node, uint32_t offset) {
  if (!node)
    return;
  spinlock_acquire(&node->pages_lock);
  vfs_page_t *page = radix_tree_lookup(&node->pages, cache_key(offset));
  if (page && page->uptodate && !page->evicted) {
    page->dirty = true;
    page->dirty_seq++;
    page->last_used = cache_stamp();
  }
  spinlock_release(&node->pages_lock);
}

struct dirty_search {
  vfs_page_t *page;
  uint64_t dirty_seq;
};

static bool find_dirty(uint64_t key, void *value, void *opaque) {
  (void)key;
  struct dirty_search *search = opaque;
  vfs_page_t *page = value;
  if (!page->dirty || page->loading || page->writeback)
    return true;
  page->writeback = true;
  page->refs++;
  search->page = page;
  search->dirty_seq = page->dirty_seq;
  return false;
}

void vfs_cache_sync(vfs_node_t *node) {
  if (!node || !node->write)
    return;
  while (1) {
    struct dirty_search search = {0};
    spinlock_acquire(&node->pages_lock);
    radix_tree_for_each(&node->pages, find_dirty, &search);
    spinlock_release(&node->pages_lock);
    if (!search.page)
      break;
    uint32_t available = node->length > search.page->offset
                             ? node->length - search.page->offset
                             : 0;
    uint32_t amount = available > PAGE_SIZE ? PAGE_SIZE : available;
    uint32_t written = amount ? node->write(
                                    node, search.page->offset, amount,
                                    PHYS_TO_VIRT(search.page->frame_phys))
                              : 0;
    spinlock_acquire(&node->pages_lock);
    if (written == amount && search.page->dirty_seq == search.dirty_seq)
      search.page->dirty = false;
    search.page->writeback = false;
    bool failed = written != amount;
    spinlock_release(&node->pages_lock);
    vfs_cache_put(node, search.page);
    if (failed)
      break;
  }
}

bool vfs_cache_phase3_stress_test(void) {
  vfs_node_t node;
  vfs_node_init(&node);
  size_t cached_before = vfs_cache_page_count();
  uint64_t frames[128] = {0};
  bool pass = true;

  for (uint32_t i = 0; i < 128; i++) {
    void *frame = pmm_alloc_page();
    if (!frame) {
      pass = false;
      break;
    }
    uint32_t offset = i * 17u * PAGE_SIZE;
    frames[i] = (uint64_t)frame;
    vfs_page_t *page = vfs_cache_insert(&node, offset, (uint64_t)frame);
    if (!page || page->offset != offset) {
      if (!page)
        pmm_free_page(frame);
      pass = false;
      break;
    }
    vfs_cache_put(&node, page);
  }

  for (uint32_t operation = 0; pass && operation < 100000; operation++) {
    uint32_t slot = (operation * 73u) & 127u;
    uint32_t offset = slot * 17u * PAGE_SIZE;
    vfs_page_t *page = vfs_cache_lookup(&node, offset);
    if (!page || page->offset != offset)
      pass = false;
    if (page)
      vfs_cache_put(&node, page);
  }

  /* A duplicate insertion must preserve the existing page and consume the
   * caller's redundant frame, matching the cache insertion contract. */
  void *duplicate_frame = pmm_alloc_page();
  vfs_page_t *original = vfs_cache_lookup(&node, 126u * 17u * PAGE_SIZE);
  vfs_page_t *duplicate = duplicate_frame
      ? vfs_cache_insert(&node, 126u * 17u * PAGE_SIZE,
                         (uint64_t)duplicate_frame)
      : NULL;
  if (!original || duplicate != original ||
      pmm_get_ref(duplicate_frame) != 0)
    pass = false;
  if (duplicate)
    vfs_cache_put(&node, duplicate);
  if (original)
    vfs_cache_put(&node, original);

  /* Invalidation removes lookup visibility immediately, but a held cache
   * value and its frame remain valid until the final put. */
  vfs_page_t *held = vfs_cache_lookup(&node, 127u * 17u * PAGE_SIZE);
  if (!held) {
    pass = false;
  } else {
    uint64_t held_frame = held->frame_phys;
    vfs_cache_invalidate(&node, held->offset);
    vfs_page_t *gone = vfs_cache_lookup(&node, held->offset);
    if (gone || !held->evicted || pmm_get_ref((void *)held_frame) != 1)
      pass = false;
    if (gone)
      vfs_cache_put(&node, gone);
    vfs_cache_put(&node, held);
    if (pmm_get_ref((void *)held_frame) != 0)
      pass = false;
  }

  vfs_cache_invalidate_range(&node, 0, 64u * 17u * PAGE_SIZE);
  for (uint32_t i = 0; pass && i < 128; i++) {
    uint32_t offset = i * 17u * PAGE_SIZE;
    vfs_page_t *page = vfs_cache_lookup(&node, offset);
    bool should_exist = i >= 64 && i != 127;
    if (!!page != should_exist)
      pass = false;
    if (page)
      vfs_cache_put(&node, page);
  }

  vfs_cache_clear(&node);
  for (uint32_t i = 0; i < 128; i++) {
    if (frames[i] && pmm_get_ref((void *)frames[i]) != 0)
      pass = false;
  }
  return pass && radix_tree_validate(&node.pages) &&
         vfs_cache_page_count() == cached_before;
}

struct phase4_file {
  uint8_t *data;
  uint32_t length;
  uint32_t read_calls;
  uint32_t write_calls;
};

static uint32_t phase4_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  struct phase4_file *file = node->device;
  file->read_calls++;
  if (offset >= file->length)
    return 0;
  if (size > file->length - offset)
    size = file->length - offset;
  memcpy(buffer, file->data + offset, size);
  return size;
}

static uint32_t phase4_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  struct phase4_file *file = node->device;
  file->write_calls++;
  if (offset >= file->length)
    return 0;
  if (size > file->length - offset)
    size = file->length - offset;
  memcpy(file->data + offset, buffer, size);
  return size;
}

bool vfs_cache_phase4_stress_test(void) {
  const uint32_t file_size = 3u * PAGE_SIZE + 123u;
  const uint32_t read_offset = PAGE_SIZE - 37u;
  const uint32_t read_size = PAGE_SIZE + 211u;
  size_t cached_before = vfs_cache_page_count();
  struct phase4_file file = {0};
  vfs_node_t node;
  bool pass = true;

  file.data = kmalloc(file_size);
  uint8_t *buffer = kmalloc(read_size);
  uint8_t *second = kmalloc(read_size);
  if (!file.data || !buffer || !second) {
    if (file.data)
      kfree(file.data);
    if (buffer)
      kfree(buffer);
    if (second)
      kfree(second);
    return false;
  }
  file.length = file_size;
  for (uint32_t i = 0; i < file_size; i++)
    file.data[i] = (uint8_t)((i * 29u + (i >> 5) + 17u) & 0xffu);

  vfs_node_init(&node);
  node.flags = FS_FILE | FS_PAGE_CACHE;
  node.length = file_size;
  node.device = &file;
  node.read = phase4_read;
  node.write = phase4_write;

  if (vfs_read(&node, read_offset, read_size, buffer) != read_size ||
      memcmp(buffer, file.data + read_offset, read_size))
    pass = false;
  uint32_t reads_after_fill = file.read_calls;
  for (uint32_t i = 0; pass && i < 100000u; i++) {
    if (vfs_read(&node, read_offset, read_size, second) != read_size ||
        memcmp(second, buffer, read_size))
      pass = false;
  }
  if (file.read_calls != reads_after_fill || reads_after_fill != 3u)
    pass = false;

  vfs_page_t *mapped = vfs_cache_get_or_create(&node, PAGE_SIZE);
  vfs_page_t *shared = vfs_cache_lookup(&node, PAGE_SIZE);
  if (!mapped || !shared || mapped->frame_phys != shared->frame_phys)
    pass = false;
  if (shared)
    vfs_cache_put(&node, shared);
  if (mapped)
    vfs_cache_put(&node, mapped);

  if (vfs_read(&node, file_size - 19u, 64u, second) != 19u ||
      memcmp(second, file.data + file_size - 19u, 19u))
    pass = false;
  vfs_page_t *past_eof = vfs_cache_lookup(&node, file_size + PAGE_SIZE);
  if (past_eof)
    pass = false;
  if (past_eof)
    vfs_cache_put(&node, past_eof);

  uint8_t replacement[73];
  memset(replacement, 0xa5, sizeof(replacement));
  uint32_t reads_before_write = file.read_calls;
  if (vfs_write(&node, PAGE_SIZE + 9u, sizeof(replacement), replacement) !=
          sizeof(replacement) ||
      file.write_calls != 1u ||
      vfs_read(&node, PAGE_SIZE + 9u, sizeof(replacement), second) !=
          sizeof(replacement) ||
      memcmp(second, replacement, sizeof(replacement)) ||
      file.read_calls != reads_before_write + 1u)
    pass = false;

  vfs_cache_clear(&node);
  pass = pass && radix_tree_validate(&node.pages) &&
         vfs_cache_page_count() == cached_before;
  kfree(second);
  kfree(buffer);
  kfree(file.data);
  return pass;
}

struct phase5_file {
  uint32_t writes;
  uint32_t last_offset;
  uint8_t first_bytes[64];
  bool fail_writes;
};

static uint32_t phase5_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  (void)node;
  for (uint32_t i = 0; i < size; i++)
    buffer[i] = (uint8_t)(((offset + i) * 13u + 41u) & 0xffu);
  return size;
}

static uint32_t phase5_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  struct phase5_file *file = node->device;
  file->writes++;
  file->last_offset = offset;
  uint32_t copy = size < sizeof(file->first_bytes) ? size
                                                   : sizeof(file->first_bytes);
  memcpy(file->first_bytes, buffer, copy);
  return file->fail_writes ? 0 : size;
}

bool vfs_cache_phase5_stress_test(void) {
  const uint32_t pages = CACHE_NODE_LIMIT + 64u;
  size_t cached_before = vfs_cache_page_count();
  struct phase5_file file = {0};
  vfs_node_t node;
  bool pass = true;

  vfs_node_init(&node);
  node.flags = FS_FILE | FS_PAGE_CACHE;
  node.length = pages * PAGE_SIZE;
  node.device = &file;
  node.read = phase5_read;
  node.write = phase5_write;

  vfs_page_t *held = vfs_cache_get_or_create(&node, 0);
  if (!held)
    pass = false;
  for (uint32_t i = 1; pass && i < pages; i++) {
    vfs_page_t *page = vfs_cache_get_or_create(&node, i * PAGE_SIZE);
    if (!page)
      pass = false;
    if (page)
      vfs_cache_put(&node, page);
    spinlock_acquire(&node.pages_lock);
    size_t excess = node.pages.entries > CACHE_NODE_LIMIT
                        ? (size_t)(node.pages.entries - CACHE_NODE_LIMIT)
                        : 0;
    spinlock_release(&node.pages_lock);
    if (excess && vfs_cache_reclaim(&node, excess) != excess)
      pass = false;
  }
  spinlock_acquire(&node.pages_lock);
  uint64_t bounded_entries = node.pages.entries;
  spinlock_release(&node.pages_lock);
  vfs_page_t *held_lookup = vfs_cache_lookup(&node, 0);
  if (bounded_entries > CACHE_NODE_LIMIT || !held_lookup)
    pass = false;
  if (held_lookup)
    vfs_cache_put(&node, held_lookup);
  if (held)
    vfs_cache_put(&node, held);

  uint32_t dirty_offset = (pages - 1u) * PAGE_SIZE;
  vfs_page_t *dirty = vfs_cache_get_or_create(&node, dirty_offset);
  uint8_t expected[64];
  memset(expected, 0x6d, sizeof(expected));
  if (!dirty) {
    pass = false;
  } else {
    memcpy(PHYS_TO_VIRT(dirty->frame_phys), expected, sizeof(expected));
    vfs_cache_mark_dirty(&node, dirty_offset);
    vfs_cache_put(&node, dirty);
  }

  file.fail_writes = true;
  vfs_cache_sync(&node);
  dirty = vfs_cache_lookup(&node, dirty_offset);
  if (!dirty || !dirty->dirty || file.writes != 1u)
    pass = false;
  if (dirty)
    vfs_cache_put(&node, dirty);

  file.fail_writes = false;
  vfs_cache_sync(&node);
  dirty = vfs_cache_lookup(&node, dirty_offset);
  if (!dirty || dirty->dirty || file.writes != 2u ||
      file.last_offset != dirty_offset ||
      memcmp(file.first_bytes, expected, sizeof(expected)))
    pass = false;
  if (dirty)
    vfs_cache_put(&node, dirty);

  spinlock_acquire(&node.pages_lock);
  size_t remaining = (size_t)node.pages.entries;
  spinlock_release(&node.pages_lock);
  if (vfs_cache_reclaim(&node, remaining) != remaining)
    pass = false;
  vfs_cache_clear(&node);
  return pass && radix_tree_validate(&node.pages) &&
         vfs_cache_page_count() == cached_before;
}
