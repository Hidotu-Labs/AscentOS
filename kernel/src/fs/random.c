#include "random.h"
#include "../fb/framebuffer.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "vfs.h"

static inline uint64_t __random_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static uint32_t random_read(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  for (uint32_t i = 0; i < size; i++) {
    uint64_t ticks = __random_rdtsc();
    // Minimal mixing to satisfy WolfSSL requirement
    buffer[i] = (uint8_t)(ticks ^ (ticks >> 7) ^ (ticks >> 17) ^ (ticks >> 23));
  }
  return size;
}

void random_register_vfs(void) {
  vfs_node_t *unode = kmalloc(sizeof(vfs_node_t));
  if (!unode)
    return;
  vfs_node_init(unode);
  strcpy(unode->name, "urandom");
  unode->flags = FS_CHARDEV;
  unode->mask = 0666;
  unode->read = random_read;
  fb_register_device_node("urandom", unode);

  vfs_node_t *rnode = kmalloc(sizeof(vfs_node_t));
  if (!rnode)
    return;
  vfs_node_init(rnode);
  strcpy(rnode->name, "random");
  rnode->flags = FS_CHARDEV;
  rnode->mask = 0666;
  rnode->read = random_read;
  fb_register_device_node("random", rnode);
}
