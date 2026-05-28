#include "vfs.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../mm/heap.h"

vfs_node_t *fs_root = 0;

typedef struct vfs_mount_entry {
  vfs_node_t *mountpoint;
  vfs_node_t *target;
  struct vfs_mount_entry *next;
} vfs_mount_entry_t;

static vfs_mount_entry_t *vfs_mount_list = NULL;

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buffer) {
  if (node && node->read) {
    uint32_t ret = node->read(node, offset, size, buffer);
    /* if (ret == 0 && size > 0) {
      klog_puts("[VFS] node->read returned 0 node=");
      klog_uint64((uint64_t)node);
      klog_puts(" name=");
      klog_puts(node->name);
      klog_puts("\n");
    } */
    return ret;
  }
  if (node && !node->read) {
    klog_puts("[VFS] node->read is NULL node=");
    klog_uint64((uint64_t)node);
    klog_puts(" name=");
    klog_puts(node->name);
    klog_puts("\n");
  }
  return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer) {
  if (node && node->write) {
    uint32_t written = node->write(node, offset, size, buffer);
    if (written > 0 && node->flags != FS_PIPE) {
      // Filter out common high-volume writes if needed, but for now log
      // klog_puts("[VFS] node write successful\n");
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
    if (node->close) {
      node->close(node);
    }
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

    // Check if this inode/dev combo or path is a mountpoint
    // For simplicity, we check if the returned node matches any mountpoint
    // in the global list. Since lookups return new nodes (like in ext2),
    // we should match by inode (and eventually device id).
    vfs_mount_entry_t *curr = vfs_mount_list;
    while (curr) {
      if (curr->mountpoint->inode == res->inode &&
          curr->mountpoint->device == res->device) {
        // If it's the root directory of the same mount, don't recurse
        if (curr->target != res) {
          // kfree(res); // Discarding the 'covered' node is risky without
          // refcounts
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
    return node->truncate(node, size);
  }
  return -1;
}

int vfs_poll(vfs_node_t *node, int events) {
  if (node && node->poll) {
    return node->poll(node, events);
  }
  // Default: if no poll handler, assume ready for regular files
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

  // Parent stack so ".." can walk up correctly even in ramfs nodes that
  // return self for "..".  Depth of 32 is more than enough for any real path.
#define VFS_PARENT_STACK_DEPTH 32
  vfs_node_t *parent_stack[VFS_PARENT_STACK_DEPTH];
  int stack_top = 0;
  parent_stack[0] = current;

  // Initial skip of root slashes
  if (path_buf[0] == '/') {
    while (*p == '/')
      p++;
  }

  while (*p) {
    char comp[128];
    int i = 0;

    // Skip slashes
    while (*p == '/')
      p++;
    if (*p == '\0')
      break;

    // Extract next component
    while (*p && *p != '/' && i < 127) {
      comp[i++] = *p++;
    }
    comp[i] = '\0';

    // Handle ".." by popping the parent stack instead of asking the fs,
    // because ramfs_finddir returns self for ".." (no parent pointer stored).
    if (strcmp(comp, "..") == 0) {
      if (stack_top > 0)
        stack_top--;
      current = parent_stack[stack_top];
      continue;
    }

    // Handle "." — stay in place
    if (strcmp(comp, ".") == 0)
      continue;

    vfs_node_t *next = vfs_finddir(current, comp);
    if (!next) {
      if (current != fs_root && current != dir)
        if (!(current->flags & FS_PERSISTENT))
          kfree(current);
      kfree(path_buf);
      return 0;
    }

    // Handle symlinks
    if ((next->flags & FS_TYPE_MASK) == FS_SYMLINK) {
      if (++symlink_depth > MAX_SYMLINK_DEPTH) {
        kfree(next);
        if (current != fs_root && current != dir)
          if (!(current->flags & FS_PERSISTENT))
            kfree(current);
        kfree(path_buf);
        return 0;
      }

      char link_target[256];
      int len = vfs_readlink(next, link_target, 256);
      kfree(next);

      if (len < 0) {
        if (current != fs_root && current != dir)
          if (!(current->flags & FS_PERSISTENT))
            kfree(current);
        kfree(path_buf);
        return 0;
      }

      // Construct new path: [link_target] + "/" + [remaining p]
      char *next_path = kmalloc(512);
      if (!next_path) {
        if (current != fs_root && current != dir)
          if (!(current->flags & FS_PERSISTENT))
            kfree(current);
        kfree(path_buf);
        return 0;
      }
      strncpy(next_path, link_target, 511);
      next_path[511] = '\0';

      if (*p) {
        int cur_len = strlen(next_path);
        if (cur_len < 510) {
          if (next_path[cur_len - 1] != '/') {
            strcat(next_path, "/");
          }
          strncat(next_path, p, 511 - strlen(next_path));
        }
      }
      next_path[511] = '\0';

      // Update path_buf and p
      strcpy(path_buf, next_path);
      kfree(next_path);
      p = path_buf;

      if (path_buf[0] == '/') {
        if (current != fs_root && current != dir)
          if (!(current->flags & FS_PERSISTENT))
            kfree(current);
        current = fs_root;
        // Reset parent stack to root for absolute symlink targets
        stack_top = 0;
        parent_stack[0] = fs_root;
        while (*p == '/')
          p++;
      }
      // Continue loop with new path and same current (if relative) or root (if
      // absolute)
      continue;
    }

    // Move to next directory component
    if (current != fs_root && current != dir &&
        !(current->flags & FS_PERSISTENT)) {
      kfree(current);
    }
    current = next;
    // Push onto parent stack
    if (stack_top < VFS_PARENT_STACK_DEPTH - 1)
      stack_top++;
    parent_stack[stack_top] = current;
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
  node->refcount = 1;
}

int vfs_mount(vfs_node_t *mountpoint, vfs_node_t *target) {
  if (!mountpoint || !target)
    return -1;

  vfs_mount_entry_t *entry = kmalloc(sizeof(vfs_mount_entry_t));
  entry->mountpoint = mountpoint;
  entry->target = target;
  entry->next = vfs_mount_list;
  vfs_mount_list = entry;

  mountpoint->flags |= FS_MOUNTPOINT | FS_PERSISTENT;
  mountpoint->ptr = target;

  return 0;
}
