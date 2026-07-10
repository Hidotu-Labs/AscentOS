#ifndef FS_EXT4_DIR_INDEX_H
#define FS_EXT4_DIR_INDEX_H

#include "fs/ext2/ext2.h"

int ext4_dx_add_entry(ext2_mount_t *mnt, uint32_t dir_inode_num,
                      ext2_inode_t *dir_inode, uint32_t child_inode_num,
                      const char *name, uint8_t file_type);

#endif
