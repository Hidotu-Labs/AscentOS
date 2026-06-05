#include "drivers/virtio/virtio.h"
#include "mm/pmm.h"
#include "console/klog.h"
#include "lib/string.h"
#include <stdint.h>

// Helpers


// Alignment helpers per VirtIO 1.0 spec
// Descriptor table:  16-byte aligned
// Available ring:     2-byte aligned
// Used ring:          4-byte aligned

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

// Calculate Descriptor Table size
static inline uint64_t virtq_desc_size(uint16_t num) {
  return (uint64_t)num * sizeof(struct virtq_desc);
}

// Calculate Available Ring size (including the used_event field)
static inline uint64_t virtq_avail_size(uint16_t num) {
  return sizeof(uint16_t) * 2 + sizeof(uint16_t) * num + sizeof(uint16_t);
}

// Calculate Used Ring size (including the avail_event field)
static inline uint64_t virtq_used_size(uint16_t num) {
  return sizeof(uint16_t) * 2 + sizeof(struct virtq_used_elem) * num +
         sizeof(uint16_t);
}

// Virtqueue Initialization

bool virtq_init(struct virtqueue *vq, uint16_t num) {
  // Calculate total size needed with proper alignment
  uint64_t desc_bytes = ALIGN_UP(virtq_desc_size(num), 16);
  uint64_t avail_bytes = ALIGN_UP(virtq_avail_size(num), 4); // align for used
  uint64_t used_bytes = ALIGN_UP(virtq_used_size(num), 4);

  uint64_t total = desc_bytes + avail_bytes + used_bytes;
  uint64_t pages_needed = (total + PAGE_SIZE - 1) / PAGE_SIZE;

  // Allocate physically contiguous pages
  void *phys = pmm_alloc_pages(pages_needed);
  if (!phys) {
    klog_puts("[VIRTIO] Failed to allocate virtqueue (");
    klog_uint64(pages_needed);
    klog_puts(" pages)\n");
    return false;
  }

  uint64_t phys_base = (uint64_t)phys;
  uint64_t virt_base = phys_base + pmm_get_hhdm_offset();

  // Zero the entire region
  memset((void *)virt_base, 0, pages_needed * PAGE_SIZE);

  // Set up pointers
  vq->desc_phys  = phys_base;
  vq->avail_phys = phys_base + desc_bytes;
  vq->used_phys  = phys_base + desc_bytes + avail_bytes;

  vq->desc  = (volatile struct virtq_desc *)(virt_base);
  vq->avail = (volatile struct virtq_avail *)(virt_base + desc_bytes);
  vq->used  = (volatile struct virtq_used *)(virt_base + desc_bytes +
                                              avail_bytes);

  vq->num = num;
  vq->free_head = 0;
  vq->num_free = num;
  vq->last_used_idx = 0;
  vq->notify_addr = NULL;

  // Build free descriptor chain
  for (uint16_t i = 0; i < num - 1; i++) {
    vq->desc[i].next = i + 1;
    vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
  }
  vq->desc[num - 1].next = 0;
  vq->desc[num - 1].flags = 0;

  // Initialize available ring
  vq->avail->flags = 0;
  vq->avail->idx = 0;

  // Initialize used ring
  vq->used->flags = 0;
  vq->used->idx = 0;

  return true;
}

// Virtqueue Buffer Operations

static int alloc_desc(struct virtqueue *vq) {
  if (vq->num_free == 0)
    return -1;

  uint16_t idx = vq->free_head;
  vq->free_head = vq->desc[idx].next;
  vq->num_free--;
  return idx;
}

int virtq_add_buf_readonly(struct virtqueue *vq, uint64_t phys_addr,
                           uint32_t len) {
  int idx = alloc_desc(vq);
  if (idx < 0) return -1;

  vq->desc[idx].addr  = phys_addr;
  vq->desc[idx].len   = len;
  vq->desc[idx].flags = 0; // Device reads (no WRITE flag)
  vq->desc[idx].next  = 0;

  return idx;
}

int virtq_add_buf_chain(struct virtqueue *vq,
                        uint64_t req_phys, uint32_t req_len,
                        uint64_t resp_phys, uint32_t resp_len) {
  // Need two descriptors
  if (vq->num_free < 2)
    return -1;

  int head = alloc_desc(vq);
  int tail = alloc_desc(vq);
  if (head < 0 || tail < 0)
    return -1;

  // Request descriptor (device reads)
  vq->desc[head].addr  = req_phys;
  vq->desc[head].len   = req_len;
  vq->desc[head].flags = VIRTQ_DESC_F_NEXT;
  vq->desc[head].next  = (uint16_t)tail;

  // Response descriptor (device writes)
  vq->desc[tail].addr  = resp_phys;
  vq->desc[tail].len   = resp_len;
  vq->desc[tail].flags = VIRTQ_DESC_F_WRITE;
  vq->desc[tail].next  = 0;

  return head;
}

void virtq_kick(struct virtqueue *vq) {
  __asm__ volatile("" ::: "memory"); // Write barrier

  // Put the descriptor head into the available ring
  // Note: the caller should have set up the descriptor already.
  // We assume the last alloc'd descriptor head was recorded by caller.
  // This is a low-level "make avail ring visible" call.

  // Actually, the typical pattern is:
  //   head = virtq_add_buf_chain(...)
  //   vq->avail->ring[vq->avail->idx % vq->num] = head
  //   vq->avail->idx++
  //   virtq_kick(vq)
  // But we provide a convenience function.

  // The memory barrier ensures the device sees the avail ring update.
  __asm__ volatile("mfence" ::: "memory");

  // Notify the device
  if (vq->notify_addr) {
    *vq->notify_addr = 0; // Queue index is typically part of the notify addr
  }
}

bool virtq_poll(struct virtqueue *vq, uint32_t *id, uint32_t *len) {
  __asm__ volatile("" ::: "memory"); // Read barrier

  if (vq->last_used_idx == vq->used->idx)
    return false;

  uint16_t used_idx = vq->last_used_idx % vq->num;
  *id = vq->used->ring[used_idx].id;
  *len = vq->used->ring[used_idx].len;
  vq->last_used_idx++;

  return true;
}

void virtq_free_desc(struct virtqueue *vq, uint16_t head) {
  uint16_t idx = head;
  while (1) {
    bool has_next = (vq->desc[idx].flags & VIRTQ_DESC_F_NEXT);
    uint16_t next = vq->desc[idx].next;

    // Return this descriptor to the free list
    vq->desc[idx].addr = 0;
    vq->desc[idx].len = 0;
    vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[idx].next = vq->free_head;
    vq->free_head = idx;
    vq->num_free++;

    if (!has_next) break;
    idx = next;
  }
}

// Self-Test

void virtio_self_test(void) {
  klog_puts("\n[VIRTIO] ═══ VirtIO Foundation Self-Test ═══\n");

  // Test 1: Virtqueue allocation
  struct virtqueue vq;
  bool ok = virtq_init(&vq, 16);
  if (!ok) {
    klog_puts("[VIRTIO] FAIL: virtq_init failed\n");
    return;
  }
  klog_puts("[VIRTIO] PASS: virtq_init(16) succeeded\n");

  // Test 2: Check initial state
  if (vq.num != 16 || vq.num_free != 16 || vq.free_head != 0) {
    klog_puts("[VIRTIO] FAIL: bad initial state\n");
    return;
  }
  klog_puts("[VIRTIO] PASS: initial state correct (num=16, free=16)\n");

  // Test 3: Physical addresses must be page-aligned
  if ((vq.desc_phys & 0xFFF) != 0) {
    klog_puts("[VIRTIO] FAIL: desc_phys not page-aligned\n");
    return;
  }
  klog_puts("[VIRTIO] PASS: desc_phys page-aligned at ");
  klog_hex64(vq.desc_phys);
  klog_puts("\n");

  // Test 4: Allocate a single read-only buffer
  uint64_t fake_phys = 0xDEADBEEF000;
  int idx = virtq_add_buf_readonly(&vq, fake_phys, 256);
  if (idx < 0) {
    klog_puts("[VIRTIO] FAIL: virtq_add_buf_readonly failed\n");
    return;
  }
  if (vq.num_free != 15) {
    klog_puts("[VIRTIO] FAIL: free count wrong after alloc\n");
    return;
  }
  klog_puts("[VIRTIO] PASS: alloc single descriptor idx=");
  klog_uint64(idx);
  klog_puts(" (free=15)\n");

  // Test 5: Allocate a request/response chain
  int head = virtq_add_buf_chain(&vq, 0x1000, 64, 0x2000, 128);
  if (head < 0) {
    klog_puts("[VIRTIO] FAIL: virtq_add_buf_chain failed\n");
    return;
  }
  if (vq.num_free != 13) {
    klog_puts("[VIRTIO] FAIL: free count wrong after chain alloc\n");
    return;
  }
  // Verify chaining
  if (!(vq.desc[head].flags & VIRTQ_DESC_F_NEXT)) {
    klog_puts("[VIRTIO] FAIL: chain head missing NEXT flag\n");
    return;
  }
  uint16_t tail_idx = vq.desc[head].next;
  if (!(vq.desc[tail_idx].flags & VIRTQ_DESC_F_WRITE)) {
    klog_puts("[VIRTIO] FAIL: chain tail missing WRITE flag\n");
    return;
  }
  klog_puts("[VIRTIO] PASS: chain desc head=");
  klog_uint64(head);
  klog_puts(" -> tail=");
  klog_uint64(tail_idx);
  klog_puts(" (free=13)\n");

  // Test 6: Free descriptors
  virtq_free_desc(&vq, (uint16_t)idx);
  if (vq.num_free != 14) {
    klog_puts("[VIRTIO] FAIL: free count wrong after free single\n");
    return;
  }
  klog_puts("[VIRTIO] PASS: freed single desc (free=14)\n");

  virtq_free_desc(&vq, (uint16_t)head);
  if (vq.num_free != 16) {
    klog_puts("[VIRTIO] FAIL: free count wrong after free chain\n");
    return;
  }
  klog_puts("[VIRTIO] PASS: freed chain desc (free=16)\n");

  // Test 7: Exhaust all descriptors
  int last = -1;
  for (int i = 0; i < 16; i++) {
    last = virtq_add_buf_readonly(&vq, 0x3000 + i * 0x1000, 64);
    if (last < 0) {
      klog_puts("[VIRTIO] FAIL: ran out of descs at i=");
      klog_uint64(i);
      klog_puts("\n");
      return;
    }
  }
  if (vq.num_free != 0) {
    klog_puts("[VIRTIO] FAIL: free should be 0\n");
    return;
  }
  // One more should fail
  int over = virtq_add_buf_readonly(&vq, 0xBAD, 1);
  if (over != -1) {
    klog_puts("[VIRTIO] FAIL: alloc should have returned -1\n");
    return;
  }
  klog_puts("[VIRTIO] PASS: exhaustion test (16/16 used, alloc returns -1)\n");

  // Clean up
  for (int i = 0; i < 16; i++) {
    virtq_free_desc(&vq, (uint16_t)i);
  }

  // Free the underlying physical pages
  uint64_t desc_bytes = ALIGN_UP(virtq_desc_size(16), 16);
  uint64_t avail_bytes = ALIGN_UP(virtq_avail_size(16), 4);
  uint64_t used_bytes = ALIGN_UP(virtq_used_size(16), 4);
  uint64_t total = desc_bytes + avail_bytes + used_bytes;
  uint64_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
  pmm_free_pages((void *)vq.desc_phys, pages);

  klog_puts("[VIRTIO] ═══ All self-tests PASSED ═══\n\n");
}
