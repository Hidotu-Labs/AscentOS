#include "vma.h"
#include "../lib/string.h"
#include "heap.h"
#include "slab_cache.h"

// ── Slab-accelerated allocation for VMA nodes ──────────────────────────────
// Falls back to kmalloc if the vma_cache hasn't been created yet (early boot).
static inline struct vma *vma_node_alloc(void) {
  if (vma_cache)
    return (struct vma *)kmem_cache_alloc(vma_cache);
  return (struct vma *)kmalloc(sizeof(struct vma));
}

static inline void vma_node_free(struct vma *v) {
  if (vma_cache)
    kmem_cache_free(vma_cache, v);
  else
    kfree(v);
}

// Helper macros
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static int get_height(struct vma *n) { return n ? n->height : 0; }

static uint64_t get_max_end(struct vma *n) { return n ? n->max_end : 0; }

static void update_node(struct vma *n) {
  if (!n)
    return;
  n->height = 1 + MAX(get_height(n->left), get_height(n->right));
  n->max_end = n->end;
  uint64_t max_left = get_max_end(n->left);
  uint64_t max_right = get_max_end(n->right);
  if (max_left > n->max_end)
    n->max_end = max_left;
  if (max_right > n->max_end)
    n->max_end = max_right;
}

static struct vma *right_rotate(struct vma *y) {
  struct vma *x = y->left;
  struct vma *T2 = x->right;

  x->right = y;
  y->left = T2;

  update_node(y);
  update_node(x);
  return x;
}

static struct vma *left_rotate(struct vma *x) {
  struct vma *y = x->right;
  struct vma *T2 = y->left;

  y->left = x;
  x->right = T2;

  update_node(x);
  update_node(y);
  return y;
}

static int get_balance(struct vma *n) {
  return n ? get_height(n->left) - get_height(n->right) : 0;
}

static struct vma *insert_node(struct vma *node, struct vma *new_node) {
  if (!node)
    return new_node;

  if (new_node->start < node->start)
    node->left = insert_node(node->left, new_node);
  else if (new_node->start > node->start)
    node->right = insert_node(node->right, new_node);
  else
    return node; // Duplicate overlapping start boundaries rejected natively
                 // inside vma_add earlier

  update_node(node);

  int balance = get_balance(node);

  // Left Left
  if (balance > 1 && new_node->start < node->left->start)
    return right_rotate(node);

  // Right Right
  if (balance < -1 && new_node->start > node->right->start)
    return left_rotate(node);

  // Left Right
  if (balance > 1 && new_node->start > node->left->start) {
    node->left = left_rotate(node->left);
    return right_rotate(node);
  }

  // Right Left
  if (balance < -1 && new_node->start < node->right->start) {
    node->right = right_rotate(node->right);
    return left_rotate(node);
  }

  return node;
}

static struct vma *min_value_node(struct vma *node) {
  struct vma *current = node;
  while (current->left != NULL)
    current = current->left;
  return current;
}

static struct vma *delete_node(struct vma *node, uint64_t start,
                               bool *deleted) {
  if (!node)
    return node;

  if (start < node->start)
    node->left = delete_node(node->left, start, deleted);
  else if (start > node->start)
    node->right = delete_node(node->right, start, deleted);
  else {
    // Node to delete found
    *deleted = true;

    if (!node->left || !node->right) {
      struct vma *temp = node->left ? node->left : node->right;
      if (!temp) {
        vma_node_free(node);
        node = NULL;
      } else {
        struct vma *unlinked = node;
        node = temp;
        vma_node_free(unlinked);
      }
    } else {
      // Node with two children: Get the inorder successor (smallest in the
      // right subtree)
      struct vma *temp = min_value_node(node->right);

      // Copy the inorder successor's data to this node
      node->start = temp->start;
      node->end = temp->end;
      node->prot = temp->prot;
      node->flags = temp->flags;
      node->offset = temp->offset;
      node->fd = temp->fd;
      node->file_node = temp->file_node;

      // Delete the inorder successor
      node->right = delete_node(node->right, temp->start, deleted);
    }
  }

  if (!node)
    return node;

  update_node(node);
  int balance = get_balance(node);

  // Left Left
  if (balance > 1 && get_balance(node->left) >= 0)
    return right_rotate(node);

  // Left Right
  if (balance > 1 && get_balance(node->left) < 0) {
    node->left = left_rotate(node->left);
    return right_rotate(node);
  }

  // Right Right
  if (balance < -1 && get_balance(node->right) <= 0)
    return left_rotate(node);

  // Right Left
  if (balance < -1 && get_balance(node->right) > 0) {
    node->right = right_rotate(node->right);
    return left_rotate(node);
  }

  return node;
}

void vma_list_init(struct vma_list *list) {
  list->root = NULL;
  list->count = 0;
}

static void vma_destroy_recursive(struct vma *node) {
  if (!node)
    return;
  vma_destroy_recursive(node->left);
  vma_destroy_recursive(node->right);
  vma_node_free(node);
}

void vma_list_destroy(struct vma_list *list) {
  vma_destroy_recursive(list->root);
  list->root = NULL;
  list->count = 0;
}

int vma_add(struct vma_list *list, uint64_t start, uint64_t end, uint64_t prot,
            uint64_t flags, int fd, uint64_t offset, void *file_node) {
  if (vma_find_overlap(list, start, end)) {
    return -1; // Overlapping regions rejected securely
  }

  struct vma *new_node = vma_node_alloc();
  if (!new_node)
    return -1; // OOM

  new_node->start = start;
  new_node->end = end;
  new_node->max_end = end;
  new_node->prot = prot;
  new_node->flags = flags;
  new_node->offset = offset;
  new_node->fd = fd;
  new_node->file_node = file_node;
  new_node->height = 1;
  new_node->left = NULL;
  new_node->right = NULL;

  list->root = insert_node(list->root, new_node);
  list->count++;
  return 0; // Success
}

bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end) {
  bool overall_removed = false;
  struct vma *v;

  // Continuously find and process overlapping regions recursively
  while ((v = vma_find_overlap(list, start, end)) != NULL) {
    overall_removed = true;

    // Save current properties before deleting the structure
    uint64_t v_start = v->start;
    uint64_t v_end = v->end;
    uint64_t prot = v->prot;
    uint64_t flags = v->flags;
    int fd = v->fd;
    uint64_t offset = v->offset;
    void *vma_file_node = v->file_node;

    bool deleted = false;
    list->root = delete_node(list->root, v->start, &deleted);
    list->count--;

    // Case 1: Complete overlap - node is entirely subsumed -> do not re-insert
    // anything

    // Case 2: Unmap from middle - split into two flanking regions
    if (start > v_start && end < v_end) {
      vma_add(list, v_start, start, prot, flags, fd, offset, vma_file_node);
      vma_add(list, end, v_end, prot, flags, fd, offset + (end - v_start),
              vma_file_node);
    }
    // Case 3: Unmap from start - shrinking start boundary forward
    else if (start <= v_start && end > v_start && end < v_end) {
      vma_add(list, end, v_end, prot, flags, fd, offset + (end - v_start),
              vma_file_node);
    }
    // Case 4: Unmap from end - shrinking end boundary backward
    else if (end >= v_end && start > v_start && start < v_end) {
      vma_add(list, v_start, start, prot, flags, fd, offset, vma_file_node);
    }
  }

  return overall_removed;
}

int vma_mprotect(struct vma_list *list, uint64_t start, uint64_t end,
                 uint64_t new_prot) {
  uint64_t curr = start;
  while (curr < end) {
    struct vma *v = vma_find(list, curr);
    if (!v) {
      // Gap found. standard mprotect returns ENOMEM in this case if it hits a
      // gap.
      v = vma_find_overlap(list, curr, end);
      if (!v)
        break;
      curr = v->start;
      continue;
    }

    uint64_t m_start = MAX(v->start, start);
    uint64_t m_end = MIN(v->end, end);

    // Save attributes
    uint64_t flags = v->flags;
    int fd = v->fd;
    uint64_t offset = v->offset;
    void *vma_file_node = v->file_node;
    uint64_t original_start = v->start;

    // Remove the overlapping part. vma_remove handles splitting the original
    // VMA.
    vma_remove(list, m_start, m_end);

    // Re-insert with new prot. The offset must be adjusted based on where
    // this segment started relative to the original VMA.
    vma_add(list, m_start, m_end, new_prot, flags, fd,
            offset + (m_start - original_start), vma_file_node);

    curr = m_end;
  }
  return 0;
}

static struct vma *vma_find_recursive(struct vma *node, uint64_t addr) {
  if (!node)
    return NULL;
  if (addr >= node->start && addr < node->end)
    return node;

  if (node->left && node->left->max_end > addr) {
    struct vma *res = vma_find_recursive(node->left, addr);
    if (res)
      return res;
  }

  return vma_find_recursive(node->right, addr);
}

struct vma *vma_find(struct vma_list *list, uint64_t addr) {
  return vma_find_recursive(list->root, addr);
}

static struct vma *vma_find_overlap_recursive(struct vma *node, uint64_t start,
                                              uint64_t end) {
  if (!node)
    return NULL;

  if (node->start < end && node->end > start)
    return node;

  if (node->left && node->left->max_end > start) {
    struct vma *res = vma_find_overlap_recursive(node->left, start, end);
    if (res)
      return res;
  }

  return vma_find_overlap_recursive(node->right, start, end);
}

struct vma *vma_find_overlap(struct vma_list *list, uint64_t start,
                             uint64_t end) {
  return vma_find_overlap_recursive(list->root, start, end);
}

static struct vma *vma_find_growdown_recursive(struct vma *node, uint64_t cr2,
                                               uint64_t max_limit) {
  if (!node)
    return NULL;

  if ((node->flags & MAP_GROWSDOWN) && cr2 < node->start &&
      cr2 >= (node->end - max_limit))
    return node;

  struct vma *res = vma_find_growdown_recursive(node->left, cr2, max_limit);
  if (res)
    return res;

  return vma_find_growdown_recursive(node->right, cr2, max_limit);
}

struct vma *vma_find_growdown(struct vma_list *list, uint64_t cr2,
                              uint64_t max_limit) {
  return vma_find_growdown_recursive(list->root, cr2, max_limit);
}

static void clone_recursive(struct vma_list *dst, struct vma *node) {
  if (!node)
    return;
  vma_add(dst, node->start, node->end, node->prot, node->flags, node->fd,
          node->offset, node->file_node);
  clone_recursive(dst, node->left);
  clone_recursive(dst, node->right);
}

void vma_list_clone(struct vma_list *dst, struct vma_list *src) {
  vma_list_init(dst);
  clone_recursive(dst, src->root);
}

static void inorder_gather(struct vma *node, struct vma **array, int *index) {
  if (!node)
    return;
  inorder_gather(node->left, array, index);
  array[*index] = node;
  (*index)++;
  inorder_gather(node->right, array, index);
}

uint64_t vma_find_gap(struct vma_list *list, uint64_t length,
                      uint64_t base_addr, uint64_t limit_addr) {
  if (list->count == 0) {
    if (base_addr + length <= limit_addr)
      return base_addr;
    return 0;
  }

  struct vma **arr = kmalloc(sizeof(struct vma *) * list->count);
  if (!arr)
    return 0;

  int idx = 0;
  inorder_gather(list->root, arr, &idx);

  uint64_t current = base_addr;
  uint64_t gap_start = 0;

  for (int i = 0; i < idx; i++) {
    if (arr[i]->end <= current)
      continue;

    if (arr[i]->start > current) {
      uint64_t gap = arr[i]->start - current;
      if (gap >= length) {
        gap_start = current;
        break;
      }
    }
    current = MAX(current, arr[i]->end);
  }

  kfree(arr);

  if (gap_start != 0)
    return gap_start;

  if (current + length <= limit_addr)
    return current;

  return 0;
}

void vma_merge_adjacent(struct vma_list *list) {
  if (list->count < 2)
    return;

  bool merged = true;
  while (merged) {
    merged = false;

    struct vma **arr = kmalloc(sizeof(struct vma *) * list->count);
    if (!arr)
      return;

    int idx = 0;
    inorder_gather(list->root, arr, &idx);

    for (int i = 0; i < idx - 1; i++) {
      struct vma *cur = arr[i];
      struct vma *nxt = arr[i + 1];

      if (cur->end == nxt->start && cur->prot == nxt->prot &&
          cur->flags == nxt->flags && (cur->fd == -1 && nxt->fd == -1)) {

        uint64_t old_start = cur->start;
        uint64_t old_end = nxt->end;
        uint64_t prot = cur->prot;
        uint64_t flags = cur->flags;

        // Save nxt boundaries BEFORE any tree modification.
        // The first vma_remove may free nxt's tree node (AVL
        // copy-up during two-child deletion), making nxt a
        // dangling pointer.
        uint64_t nxt_start = nxt->start;
        uint64_t nxt_end = nxt->end;

        vma_remove(list, cur->start, cur->end);
        vma_remove(list, nxt_start, nxt_end);

        vma_add(list, old_start, old_end, prot, flags, -1, 0, NULL);

        merged = true;
        break;
      }
    }

    kfree(arr);
  }
}
