#include "vfs.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

vfs_node_t *fs_root = 0;

typedef struct vfs_mount_entry {
  vfs_node_t *mountpoint;
  vfs_node_t *target;
  struct vfs_mount_entry *next;
} vfs_mount_entry_t;

static vfs_mount_entry_t *vfs_mount_list = NULL;

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buffer) {
  if (!node)
    return 0;

  // For regular files, try to serve from page cache first
  if ((node->flags & FS_TYPE_MASK) == FS_FILE) {
    uint32_t bytes_read = 0;
    while (bytes_read < size) {
      uint32_t file_offset = offset + bytes_read;
      if (file_offset >= node->length)
        break;
      uint32_t page_off = file_offset % 4096;
      uint32_t page_base = file_offset - page_off;
      uint32_t avail_in_file = node->length - file_offset;
      uint32_t to_copy = 4096 - page_off;
      if (to_copy > (size - bytes_read))
        to_copy = size - bytes_read;
      if (to_copy > avail_in_file)
        to_copy = avail_in_file;

      vfs_page_t *page = vfs_cache_lookup(node, page_base);
      if (page) {
        memcpy(buffer + bytes_read,
               (uint8_t *)PHYS_TO_VIRT(page->frame_phys) + page_off, to_copy);
        bytes_read += to_copy;
      } else {
        // Cache miss: fall back to filesystem read for the rest
        break;
      }
    }
    if (bytes_read > 0)
      return bytes_read;
  }

  if (node->read) {
    return node->read(node, offset, size, buffer);
  }
  return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer) {
  if (!node)
    return 0;

  // Write-back path: buffer writes in page cache for regular files
  if ((node->flags & FS_TYPE_MASK) == FS_FILE) {
    uint32_t bytes_written = 0;
    while (bytes_written < size) {
      uint32_t file_offset = offset + bytes_written;
      uint32_t page_off = file_offset % 4096;
      uint32_t page_base = file_offset - page_off;
      uint32_t to_copy = 4096 - page_off;
      if (to_copy > (size - bytes_written))
        to_copy = size - bytes_written;

      vfs_page_t *page = vfs_cache_get_or_create(node, page_base);
      if (page) {
        memcpy((uint8_t *)PHYS_TO_VIRT(page->frame_phys) + page_off,
               buffer + bytes_written, to_copy);
        page->dirty = true;
        bytes_written += to_copy;
      } else {
        // Fallback to direct write if cache allocation fails
        break;
      }
    }

    if (bytes_written > 0) {
      // Update file length if we wrote past the end
      if (offset + bytes_written > node->length) {
        node->length = offset + bytes_written;
      }
      return bytes_written;
    }
  }

  // Direct write fallback (non-file nodes, or cache alloc failure)
  if (node->write) {
    uint32_t written = node->write(node, offset, size, buffer);
    if (written > 0 && (node->flags & FS_TYPE_MASK) == FS_FILE) {
      vfs_cache_invalidate_range(node, offset, written);
    }
    return written;
  }
  return 0;
}

void vfs_open(vfs_node_t *node) {
  if (!node)
    return;
  node->refcount++;
  if (node->open) {
    node->open(node);
  }
}

void vfs_close(vfs_node_t *node) {
  if (!node)
    return;
  if (--node->refcount == 0) {
    if ((node->flags & FS_TYPE_MASK) == FS_FILE) {
      vfs_cache_sync(node);
    }
    if (node->close) {
      node->close(node);
    }
    vfs_cache_clear(node);
    if (!(node->flags & FS_PERSISTENT)) {
      kfree(node);
    }
  }
}

struct dirent *vfs_readdir(vfs_node_t *node, uint32_t index) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->readdir) {
    return node->readdir(node, index);
  }
  return 0;
}

vfs_node_t *vfs_finddir(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->finddir) {
    vfs_node_t *res = node->finddir(node, name);
    if (!res)
      return 0;

    vfs_mount_entry_t *curr = vfs_mount_list;
    while (curr) {
      if (curr->mountpoint->inode == res->inode &&
          curr->mountpoint->device == res->device) {
        if (curr->target != res) {
          return curr->target;
        }
      }
      curr = curr->next;
    }
    return res;
  }
  return 0;
}

int vfs_create(vfs_node_t *node, char *name, uint16_t permission) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->create) {
    return node->create(node, name, permission);
  }
  return -1;
}

int vfs_mkdir(vfs_node_t *node, char *name, uint16_t permission) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->mkdir) {
    return node->mkdir(node, name, permission);
  }
  return -1;
}

int vfs_unlink(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->unlink) {
    return node->unlink(node, name);
  }
  return -1;
}

int vfs_rmdir(vfs_node_t *node, char *name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->rmdir) {
    return node->rmdir(node, name);
  }
  return -1;
}

int vfs_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  if (node && node->readlink) {
    return node->readlink(node, buf, size);
  }
  return -1;
}

int vfs_symlink(vfs_node_t *node, char *name, char *target) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->symlink) {
    return node->symlink(node, name, target);
  }
  return -1;
}

int vfs_rename(vfs_node_t *node, char *old_name, char *new_name) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->rename) {
    return node->rename(node, old_name, new_name);
  }
  return -1;
}

int vfs_chmod(vfs_node_t *node, uint16_t permission) {
  if (node && node->chmod) {
    return node->chmod(node, permission);
  }
  return -1;
}

int vfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid) {
  if (node && node->chown) {
    return node->chown(node, uid, gid);
  }
  return -1;
}

int vfs_mknod(vfs_node_t *node, char *name, uint16_t permission, uint32_t flags,
              void *device) {
  if ((node->flags & FS_TYPE_MASK) == FS_DIRECTORY && node->mknod) {
    return node->mknod(node, name, permission, flags, device);
  }
  return -1;
}

int vfs_truncate(vfs_node_t *node, uint32_t size) {
  if (node && node->truncate) {
    int res = node->truncate(node, size);
    if (res == 0 && (node->flags & FS_TYPE_MASK) == FS_FILE) {
      vfs_cache_invalidate_range(node, size, node->length - size + 4096);
    }
    return res;
  }
  return -1;
}

int vfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len) {
  if (node && node->fallocate) {
    return node->fallocate(node, mode, offset, len);
  }
  return -1;
}

int vfs_poll(vfs_node_t *node, int events) {
  if (node && node->poll) {
    return node->poll(node, events);
  }
  uint32_t type = (node->flags & FS_TYPE_MASK);
  if (type == FS_FILE || type == FS_DIRECTORY) {
    return events & (POLLIN | POLLOUT);
  }
  return 0;
}

#define MAX_SYMLINK_DEPTH 8

vfs_node_t *vfs_resolve_path_at(vfs_node_t *dir, const char *path) {
  if (!path || !fs_root)
    return 0;

  char *path_buf = kmalloc(512);
  if (!path_buf)
    return 0;
  strncpy(path_buf, path, 511);
  path_buf[511] = '\0';

  vfs_node_t *current = (path[0] == '/') ? fs_root : (dir ? dir : fs_root);
  int symlink_depth = 0;
  char *p = path_buf;

#define VFS_PARENT_STACK_DEPTH 32
  vfs_node_t *parent_stack[VFS_PARENT_STACK_DEPTH];
  int stack_top = 0;
  parent_stack[0] = current;

  if (path_buf[0] == '/') {
    while (*p == '/')
      p++;
  }

  while (*p) {
    char comp[128];
    int i = 0;

    while (*p == '/')
      p++;
    if (*p == '\0')
      break;

    while (*p && *p != '/' && i < 127) {
      comp[i++] = *p++;
    }
    comp[i] = '\0';

    if (strcmp(comp, "..") == 0) {
      if (stack_top > 0) {
        vfs_node_t *to_free = current;
        stack_top--;
        current = parent_stack[stack_top];
        if (to_free != fs_root && to_free != dir && to_free != current &&
            !(to_free->flags & FS_PERSISTENT)) {
          kfree(to_free);
        }
      }
      continue;
    }

    if (strcmp(comp, ".") == 0)
      continue;

    vfs_node_t *next = vfs_finddir(current, comp);
    if (!next) {
      for (int j = 1; j <= stack_top; j++) {
        if (parent_stack[j] != fs_root && parent_stack[j] != dir &&
            !(parent_stack[j]->flags & FS_PERSISTENT)) {
          kfree(parent_stack[j]);
        }
      }
      kfree(path_buf);
      return 0;
    }

    if ((next->flags & FS_TYPE_MASK) == FS_SYMLINK) {
      if (++symlink_depth > MAX_SYMLINK_DEPTH) {
        if (!(next->flags & FS_PERSISTENT))
          kfree(next);
        for (int j = 1; j <= stack_top; j++) {
          if (parent_stack[j] != fs_root && parent_stack[j] != dir &&
              !(parent_stack[j]->flags & FS_PERSISTENT)) {
            kfree(parent_stack[j]);
          }
        }
        kfree(path_buf);
        return 0;
      }

      char link_target[512];
      int len = vfs_readlink(next, link_target, 511);
      if (!(next->flags & FS_PERSISTENT))
        kfree(next);

      if (len < 0) {
        for (int j = 1; j <= stack_top; j++) {
          if (parent_stack[j] != fs_root && parent_stack[j] != dir &&
              !(parent_stack[j]->flags & FS_PERSISTENT)) {
            kfree(parent_stack[j]);
          }
        }
        kfree(path_buf);
        return 0;
      }
      link_target[len] = '\0';

      char *next_path = kmalloc(512);
      if (!next_path) {
        for (int j = 1; j <= stack_top; j++) {
          if (parent_stack[j] != fs_root && parent_stack[j] != dir &&
              !(parent_stack[j]->flags & FS_PERSISTENT)) {
            kfree(parent_stack[j]);
          }
        }
        kfree(path_buf);
        return 0;
      }
      strcpy(next_path, link_target);

      if (*p) {
        int cur_len = (int)strlen(next_path);
        if (cur_len < 510) {
          bool target_ends_in_slash =
              (cur_len > 0 && next_path[cur_len - 1] == '/');
          bool p_starts_with_slash = (*p == '/');

          if (!target_ends_in_slash && !p_starts_with_slash) {
            strcat(next_path, "/");
          } else if (target_ends_in_slash && p_starts_with_slash) {
            p++;
          }
          strncat(next_path, p, 511 - strlen(next_path));
        }
      }
      next_path[511] = '\0';

      strcpy(path_buf, next_path);
      kfree(next_path);
      p = path_buf;

      if (path_buf[0] == '/') {
        for (int j = 1; j <= stack_top; j++) {
          if (parent_stack[j] != fs_root && parent_stack[j] != dir &&
              !(parent_stack[j]->flags & FS_PERSISTENT)) {
            kfree(parent_stack[j]);
          }
        }
        current = fs_root;
        stack_top = 0;
        parent_stack[0] = fs_root;
        while (*p == '/')
          p++;
      }
      continue;
    }

    current = next;
    if (stack_top < VFS_PARENT_STACK_DEPTH - 1) {
      stack_top++;
      parent_stack[stack_top] = current;
    } else {
      // Stack overflow - just replace current and lose parent history
      // This is better than crashing or leaking.
      // We free the previous current if it was transient.
      vfs_node_t *prev = parent_stack[stack_top];
      if (prev != fs_root && prev != dir && !(prev->flags & FS_PERSISTENT)) {
        kfree(prev);
      }
      parent_stack[stack_top] = current;
    }
  }

  for (int j = 1; j < stack_top; j++) {
    if (parent_stack[j] != current && parent_stack[j] != fs_root &&
        parent_stack[j] != dir && !(parent_stack[j]->flags & FS_PERSISTENT)) {
      kfree(parent_stack[j]);
    }
  }

  kfree(path_buf);
  return current;
}

vfs_node_t *vfs_resolve_path(const char *path) {
  return vfs_resolve_path_at(fs_root, path);
}

void vfs_node_init(vfs_node_t *node) {
  if (!node)
    return;
  memset(node, 0, sizeof(vfs_node_t));
  INIT_LIST_HEAD(&node->ep_watchers);
  spinlock_init(&node->ep_lock);
  for (int i = 0; i < 32; i++) {
    INIT_LIST_HEAD(&node->pages[i]);
  }
  spinlock_init(&node->pages_lock);
  node->refcount = 1;
}

int vfs_mount(vfs_node_t *mountpoint, vfs_node_t *target) {
  if (!mountpoint || !target)
    return -1;

  vfs_mount_entry_t *entry = kmalloc(sizeof(vfs_mount_entry_t));
  if (!entry)
    return -1;
  entry->mountpoint = mountpoint;
  entry->target = target;
  entry->next = vfs_mount_list;
  vfs_mount_list = entry;

  mountpoint->flags |= FS_MOUNTPOINT | FS_PERSISTENT;
  mountpoint->ptr = target;

  return 0;
}
