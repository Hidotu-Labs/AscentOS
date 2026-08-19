#ifndef FS_TMPFS_H
#define FS_TMPFS_H

#include "vfs.h"
#include "../lock/spinlock.h"
#include <stddef.h>
#include <stdint.h>

#define TMPFS_DEFAULT_MAX_BYTES  (64ULL * 1024 * 1024)
#define TMPFS_DEFAULT_MAX_INODES 65536
#define TMPFS_MAGIC              0x01021994

typedef struct tmpfs_page {
    uint32_t         page_index;
    uint64_t         phys;
    struct tmpfs_page *next;
} tmpfs_page_t;

typedef struct tmpfs_sb tmpfs_sb_t;

typedef struct {
    tmpfs_page_t *pages;
    uint32_t      num_pages;
    spinlock_t    lock;
    tmpfs_sb_t   *sb;
} tmpfs_file_t;

typedef struct tmpfs_child {
    vfs_node_t        *node;
    struct tmpfs_child *next;
} tmpfs_child_t;

typedef struct {
    tmpfs_child_t *children;
    tmpfs_sb_t    *sb;
} tmpfs_dir_t;

typedef struct {
    char        target[512];
    tmpfs_sb_t *sb;
} tmpfs_symlink_t;

typedef struct tmpfs_sb {
    uint64_t   max_bytes;
    uint64_t   max_inodes;
    uint64_t   used_bytes;
    uint64_t   used_inodes;
    spinlock_t lock;
} tmpfs_sb_t;

void        tmpfs_mount_at(const char *path);
void        tmpfs_mount_at_sized(const char *path, uint64_t max_bytes, uint64_t max_inodes);
vfs_node_t *tmpfs_create_root(tmpfs_sb_t *sb);

#endif
