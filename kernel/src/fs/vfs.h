#ifndef FS_VFS_H
#define FS_VFS_H

#include "../lib/list.h"
#include "../lock/spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FS_FILE 0x01
#define FS_DIRECTORY 0x02
#define FS_CHARDEV 0x03
#define FS_BLOCKDEV 0x04
#define FS_PIPE 0x05
#define FS_SYMLINK 0x06
#define FS_SOCKET 0x07
#define FS_MOUNTPOINT 0x08
#define FS_EPOLL 0x09
#define FS_PERSISTENT 0x10
#define FS_NONBLOCK 0x20
#define FS_TYPE_MASK 0x0F

// Poll Events
#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

struct vfs_node;
typedef struct vfs_page {
  uint32_t offset;       // Byte offset within the file (page aligned)
  uint64_t frame_phys;   // Physical address of the frame
  bool dirty;            // True if data has been modified but not written back
  struct list_head list; // List linkage for vfs_node_t
} vfs_page_t;

struct dirent {
  char name[128];
  uint32_t ino;
};

typedef uint32_t (*read_type_t)(struct vfs_node *, uint32_t, uint32_t,
                                uint8_t *);
typedef uint32_t (*write_type_t)(struct vfs_node *, uint32_t, uint32_t,
                                 uint8_t *);
typedef void (*open_type_t)(struct vfs_node *);
typedef void (*close_type_t)(struct vfs_node *);
typedef int (*ioctl_type_t)(struct vfs_node *, uint32_t request, uint64_t arg);
typedef struct dirent *(*readdir_type_t)(struct vfs_node *, uint32_t);
typedef struct vfs_node *(*finddir_type_t)(struct vfs_node *, char *name);
typedef int (*create_type_t)(struct vfs_node *, char *name,
                             uint16_t permission);
typedef int (*mkdir_type_t)(struct vfs_node *, char *name, uint16_t permission);
typedef int (*mknod_type_t)(struct vfs_node *, char *name, uint16_t permission,
                            uint32_t flags, void *device);
typedef int (*unlink_type_t)(struct vfs_node *, char *name);
typedef int (*rmdir_type_t)(struct vfs_node *, char *name);
typedef int (*readlink_type_t)(struct vfs_node *, char *buf, uint32_t size);
typedef int (*symlink_type_t)(struct vfs_node *, char *name, char *target);
typedef int (*rename_type_t)(struct vfs_node *, char *old_name, char *new_name);
typedef int (*chmod_type_t)(struct vfs_node *, uint16_t permission);
typedef int (*chown_type_t)(struct vfs_node *, uint32_t uid, uint32_t gid);
typedef int (*truncate_type_t)(struct vfs_node *, uint32_t);
typedef uint64_t (*mmap_type_t)(struct vfs_node *, uint64_t addr,
                                uint64_t length, uint64_t prot, uint64_t flags,
                                uint64_t offset);
typedef int (*poll_type_t)(struct vfs_node *, int events);
typedef int (*fallocate_type_t)(struct vfs_node *, int mode, uint32_t offset,
                                uint32_t len);
// statfs structure (Linux x86_64 ABI compatible)
// All fields are 'long' (8 bytes on x86_64).
// f_fsid is __kernel_fsid_t = int[2] = exactly 8 bytes — NOT uint64_t[2].
struct statfs_buf {
  int64_t  f_type;
  int64_t  f_bsize;
  int64_t  f_blocks;
  int64_t  f_bfree;
  int64_t  f_bavail;
  int64_t  f_files;
  int64_t  f_ffree;
  int32_t  f_fsid[2];   /* __kernel_fsid_t: two int32 = 8 bytes total */
  int64_t  f_namelen;
  int64_t  f_frsize;
  int64_t  f_flags;
  int64_t  f_spare[4];
};

typedef int (*statfs_type_t)(struct vfs_node *, struct statfs_buf *buf);

typedef struct vfs_node {
  char name[128];
  uint32_t mask; // Permissions
  uint32_t uid;
  uint32_t gid;
  uint32_t flags; // Node type
  uint32_t inode;
  uint32_t length; // Size of file
  uint32_t impl;   // Implementation-defined
  void *device;    // Optional binding to driver block device or ramfs specific
                   // struct

  uint32_t atime; // Access time
  uint32_t mtime; // Modification time
  uint32_t ctime; // Creation time

  read_type_t read;
  write_type_t write;
  open_type_t open;
  close_type_t close;
  readdir_type_t readdir;
  finddir_type_t finddir;
  create_type_t create;
  mkdir_type_t mkdir;
  mknod_type_t mknod;
  unlink_type_t unlink;
  rmdir_type_t rmdir;
  readlink_type_t readlink;
  symlink_type_t symlink;
  rename_type_t rename;
  chmod_type_t chmod;
  chown_type_t chown;
  truncate_type_t truncate;
  mmap_type_t mmap;   // Device-specific mmap handler
  poll_type_t poll;   // Device-specific poll handler
  statfs_type_t statfs;
  ioctl_type_t ioctl; // Device-specific ioctl handler
  fallocate_type_t fallocate;
  void *wait_queue; // Pointer to wait_queue_t for poll() wakeups

  struct list_head ep_watchers; // List of epitem_t watching this node
  spinlock_t ep_lock;           // Lock for ep_watchers

  struct vfs_node *ptr; // Used by mountpoints and symlinks
  uint32_t refcount;    // Reference count for memory management

  struct list_head pages[32]; // Hash table of cached pages (vfs_page_t)
  spinlock_t pages_lock;      // Lock for the page cache hash table
} vfs_node_t;

extern vfs_node_t *fs_root;

// Standard API wrapper functions
uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buffer);
uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer);
void vfs_open(vfs_node_t *node);
void vfs_close(vfs_node_t *node);
struct dirent *vfs_readdir(vfs_node_t *node, uint32_t index);
vfs_node_t *vfs_finddir(vfs_node_t *node, char *name);
vfs_node_t *vfs_resolve_path_at(vfs_node_t *dir, const char *path);
vfs_node_t *vfs_resolve_path(const char *path);
int vfs_create(vfs_node_t *node, char *name, uint16_t permission);
int vfs_mkdir(vfs_node_t *node, char *name, uint16_t permission);
int vfs_unlink(vfs_node_t *node, char *name);
int vfs_rmdir(vfs_node_t *node, char *name);
int vfs_readlink(vfs_node_t *node, char *buf, uint32_t size);
int vfs_symlink(vfs_node_t *node, char *name, char *target);
int vfs_rename(vfs_node_t *node, char *old_name, char *new_name);
int vfs_chmod(vfs_node_t *node, uint16_t permission);
int vfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid);
int vfs_truncate(vfs_node_t *node, uint32_t size);
int vfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len);
int vfs_mknod(vfs_node_t *node, char *name, uint16_t permission, uint32_t flags,
              void *device);
int vfs_poll(vfs_node_t *node, int events);
void vfs_node_init(vfs_node_t *node);
int vfs_mount(vfs_node_t *mountpoint, vfs_node_t *target);
int vfs_mount_ex(vfs_node_t *mountpoint, vfs_node_t *target, const char *dev_name, const char *fs_type);
int vfs_statfs(vfs_node_t *node, void *buf);

typedef struct vfs_mount_info {
  char mountpoint[128];
  char target[128];
  char dev_name[64];
  char fs_type[32];
} vfs_mount_info_t;

int vfs_get_mounts(vfs_mount_info_t *buffer, int max_count);

// Page Cache API
vfs_page_t *vfs_cache_lookup(vfs_node_t *node, uint32_t offset);
vfs_page_t *vfs_cache_insert(vfs_node_t *node, uint32_t offset, uint64_t frame);
void vfs_cache_invalidate(vfs_node_t *node, uint32_t offset);
void vfs_cache_invalidate_range(vfs_node_t *node, uint32_t offset,
                                uint32_t length);
void vfs_cache_clear(vfs_node_t *node);
void vfs_cache_sync(vfs_node_t *node);
vfs_page_t *vfs_cache_get_or_create(vfs_node_t *node, uint32_t offset);

#endif
