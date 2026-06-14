// ext2_inode.c — VFS node construction from ext2 inodes.

#include "ext2_internal.h"
#include "syscalls/syscall.h"

// Forward declarations of VFS callbacks (defined in their respective modules)
// needed when wiring up the vfs_node_t function pointers.
extern uint32_t      ext2_read_impl(vfs_node_t *, uint32_t, uint32_t, uint8_t *);
extern uint32_t      ext2_write_impl(vfs_node_t *, uint32_t, uint32_t, uint8_t *);
extern int           ext2_truncate_impl(vfs_node_t *, uint32_t);
extern uint64_t      ext2_mmap_impl(vfs_node_t *, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t);
extern struct dirent *ext2_readdir_impl(vfs_node_t *, uint32_t);
extern vfs_node_t   *ext2_finddir_impl(vfs_node_t *, char *);
extern int           ext2_create_impl(vfs_node_t *, char *, uint16_t);
extern int           ext2_mkdir_impl(vfs_node_t *, char *, uint16_t);
extern int           ext2_unlink_impl(vfs_node_t *, char *);
extern int           ext2_rmdir_impl(vfs_node_t *, char *);
extern int           ext2_readlink_impl(vfs_node_t *, char *, uint32_t);
extern int           ext2_symlink_impl(vfs_node_t *, char *, char *);
extern int           ext2_rename_impl(vfs_node_t *, char *, char *);
extern int           ext2_chmod_impl(vfs_node_t *, uint16_t);
extern int           ext2_chown_impl(vfs_node_t *, uint32_t, uint32_t);
extern int           ext2_mknod_impl(vfs_node_t *, char *, uint16_t, uint32_t,
                                     void *);

vfs_node_t *ext2_make_vfs_node(ext2_mount_t *mnt, uint32_t inode_num,
                                ext2_inode_t *inode) {
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;
  vfs_node_init(node);

  node->inode  = inode_num;
  node->mask   = inode->i_mode & 0x0FFF;
  node->uid    = inode->i_uid;
  node->gid    = inode->i_gid;
  node->length = inode->i_size;
  node->device = mnt;
  node->atime  = inode->i_atime;
  node->mtime  = inode->i_mtime;
  node->ctime  = inode->i_ctime;

  uint16_t type = inode->i_mode & 0xF000;

  if (type == EXT2_S_IFDIR) {
    node->flags   = FS_DIRECTORY;
    node->readdir = ext2_readdir_impl;
    node->finddir = ext2_finddir_impl;
    node->create  = ext2_create_impl;
    node->mkdir   = ext2_mkdir_impl;
    node->unlink  = ext2_unlink_impl;
    node->rmdir   = ext2_rmdir_impl;
    node->symlink = ext2_symlink_impl;
    node->rename  = ext2_rename_impl;
    node->chmod   = ext2_chmod_impl;
    node->chown   = ext2_chown_impl;
    node->mknod   = ext2_mknod_impl;
  } else if (type == EXT2_S_IFREG) {
    node->flags    = FS_FILE;
    node->read     = ext2_read_impl;
    node->write    = ext2_write_impl;
    node->truncate = ext2_truncate_impl;
    node->mmap     = ext2_mmap_impl;
    node->chmod    = ext2_chmod_impl;
    node->chown    = ext2_chown_impl;
  } else if (type == EXT2_S_IFLNK) {
    node->flags    = FS_SYMLINK;
    node->readlink = ext2_readlink_impl;
    node->chmod    = ext2_chmod_impl;
    node->chown    = ext2_chown_impl;
  } else if (type == EXT2_S_IFSOCK) {
    node->flags = FS_SOCKET;
    node->chmod = ext2_chmod_impl;
    node->chown = ext2_chown_impl;
  }

  return node;
}
