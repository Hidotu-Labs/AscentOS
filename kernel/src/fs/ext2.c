#include "ext2.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "ext3.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "syscalls/syscall.h"

// ── Forward declarations ────────────────────────────────────────────────────

static uint32_t ext2_read_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                               uint8_t *buffer);
static uint32_t ext2_write_impl(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer);
static int ext2_truncate_impl(vfs_node_t *node, uint32_t new_len);
static struct dirent *ext2_readdir_impl(vfs_node_t *node, uint32_t index);
static vfs_node_t *ext2_finddir_impl(vfs_node_t *node, char *name);
static int ext2_create_impl(vfs_node_t *node, char *name, uint16_t permission);
static int ext2_mkdir_impl(vfs_node_t *node, char *name, uint16_t permission);
static int ext2_unlink_impl(vfs_node_t *node, char *name);
static int ext2_rmdir_impl(vfs_node_t *node, char *name);
static int ext2_readlink_impl(vfs_node_t *node, char *buf, uint32_t size);
static int ext2_symlink_impl(vfs_node_t *node, char *name, char *target);
static int ext2_rename_impl(vfs_node_t *node, char *old_name, char *new_name);
static int ext2_chmod_impl(vfs_node_t *node, uint16_t permission);
static int ext2_chown_impl(vfs_node_t *node, uint32_t uid, uint32_t gid);
static int ext2_mknod_impl(vfs_node_t *node, char *name, uint16_t permission,
                           uint32_t flags, void *device);
static uint64_t ext2_mmap_impl(vfs_node_t *node, uint64_t addr, uint64_t length,
                               uint64_t prot, uint64_t flags, uint64_t offset);

// ── Timestamp helper ────────────────────────────────────────────────────────

// Returns seconds since boot (approximation for ext2 timestamps)
static uint32_t ext2_current_time(void) {
  return (uint32_t)(lapic_timer_get_ms() / 1000);
}

// ── Block I/O ───────────────────────────────────────────────────────────────

// Read a single ext2 block into buffer.
int ext2_read_block(ext2_mount_t *mnt, uint32_t block_num, void *buffer) {
  if (block_num == 0) {
    memset(buffer, 0, mnt->block_size);
    return 0;
  }

  // Check cache
  for (int i = 0; i < 32; i++) {
    if (mnt->cache[i].data && mnt->cache[i].num == block_num) {
      memcpy(buffer, mnt->cache[i].data, mnt->block_size);
      return 0;
    }
  }

  uint64_t byte_offset = (uint64_t)block_num * mnt->block_size;
  uint64_t lba = byte_offset / 512;
  uint32_t sectors = mnt->block_size / 512;
  int err = mnt->dev->read_sectors(mnt->dev, lba, sectors, buffer);
  if (err)
    return err;

  // Add to cache (simple round-robin replacement based on block_num)
  int idx = block_num % 32;
  if (!mnt->cache[idx].data) {
    mnt->cache[idx].data = kmalloc(mnt->block_size);
  }
  if (mnt->cache[idx].data) {
    mnt->cache[idx].num = block_num;
    memcpy(mnt->cache[idx].data, buffer, mnt->block_size);
  }

  return 0;
}

// Write a single ext2 block from buffer.
int ext2_write_block(ext2_mount_t *mnt, uint32_t block_num,
                     const void *buffer) {
  if (block_num == 0)
    return -1;

  // Update cache if present
  for (int i = 0; i < 32; i++) {
    if (mnt->cache[i].data && mnt->cache[i].num == block_num) {
      memcpy(mnt->cache[i].data, buffer, mnt->block_size);
      break;
    }
  }

  uint64_t byte_offset = (uint64_t)block_num * mnt->block_size;
  uint64_t lba = byte_offset / 512;
  uint32_t sectors = mnt->block_size / 512;
  return mnt->dev->write_sectors(mnt->dev, lba, sectors, buffer);
}

// ── Superblock / BGD persistence ────────────────────────────────────────────

static int ext2_write_superblock(ext2_mount_t *mnt) {
  // Superblock is always at byte offset 1024, spanning 1024 bytes.
  uint8_t buf[1024];
  // Read the sector(s) containing the superblock
  int err = mnt->dev->read_sectors(mnt->dev, 2, 2, buf);
  if (err)
    return -1;
  memcpy(buf, &mnt->sb, sizeof(ext2_superblock_t));
  return ext3_journal_block(
      mnt, 1, buf); // Superblock is at logical block 1 (offset 1024)
}

static int ext2_write_bgdt(ext2_mount_t *mnt) {
  uint32_t bgdt_block = mnt->sb.s_first_data_block + 1;
  uint32_t bgdt_size = mnt->groups_count * sizeof(ext2_bgd_t);
  uint32_t blocks_needed = (bgdt_size + mnt->block_size - 1) / mnt->block_size;
  uint8_t *tmp = kmalloc(blocks_needed * mnt->block_size);
  if (!tmp)
    return -1;
  memset(tmp, 0, blocks_needed * mnt->block_size);
  memcpy(tmp, mnt->bgdt, bgdt_size);
  for (uint32_t i = 0; i < blocks_needed; i++) {
    int err =
        ext3_journal_block(mnt, bgdt_block + i, tmp + i * mnt->block_size);
    if (err) {
      kfree(tmp);
      return -1;
    }
  }
  kfree(tmp);
  return 0;
}

// ── Inode I/O ───────────────────────────────────────────────────────────────

int ext2_read_inode(ext2_mount_t *mnt, uint32_t inode_num, ext2_inode_t *out) {
  if (inode_num == 0)
    return -1;
  uint32_t group = (inode_num - 1) / mnt->inodes_per_group;
  uint32_t index = (inode_num - 1) % mnt->inodes_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint32_t inode_table_block = mnt->bgdt[group].bg_inode_table;
  uint32_t byte_offset_in_table = index * mnt->inode_size;
  uint32_t block_offset = byte_offset_in_table / mnt->block_size;
  uint32_t offset_within_block = byte_offset_in_table % mnt->block_size;

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return -1;

  int err = ext2_read_block(mnt, inode_table_block + block_offset, block_buf);
  if (err) {
    kfree(block_buf);
    return -1;
  }

  memcpy(out, block_buf + offset_within_block, sizeof(ext2_inode_t));
  kfree(block_buf);
  return 0;
}

static int ext2_write_inode(ext2_mount_t *mnt, uint32_t inode_num,
                            const ext2_inode_t *inode) {
  if (inode_num == 0)
    return -1;
  uint32_t group = (inode_num - 1) / mnt->inodes_per_group;
  uint32_t index = (inode_num - 1) % mnt->inodes_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint32_t inode_table_block = mnt->bgdt[group].bg_inode_table;
  uint32_t byte_offset_in_table = index * mnt->inode_size;
  uint32_t block_offset = byte_offset_in_table / mnt->block_size;
  uint32_t offset_within_block = byte_offset_in_table % mnt->block_size;

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return -1;

  int err = ext2_read_block(mnt, inode_table_block + block_offset, block_buf);
  if (err) {
    kfree(block_buf);
    return -1;
  }

  memcpy(block_buf + offset_within_block, inode, sizeof(ext2_inode_t));

  err = ext2_write_block(mnt, inode_table_block + block_offset, block_buf);
  kfree(block_buf);
  return err;
}

// ── Block Allocation / Deallocation ─────────────────────────────────────────

// Allocate a free block from the filesystem. Returns block number or 0 on
// failure.
static uint32_t ext2_alloc_block(ext2_mount_t *mnt) {
  ext3_journal_start(mnt);
  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap)
    return 0;

  for (uint32_t g = 0; g < mnt->groups_count; g++) {
    if (mnt->bgdt[g].bg_free_blocks_count == 0)
      continue;

    ext2_read_block(mnt, mnt->bgdt[g].bg_block_bitmap, bitmap);

    uint32_t blocks_in_group = mnt->sb.s_blocks_per_group;
    // Last group may have fewer blocks
    if (g == mnt->groups_count - 1) {
      uint32_t remaining =
          mnt->sb.s_blocks_count - (g * mnt->sb.s_blocks_per_group);
      if (remaining < blocks_in_group)
        blocks_in_group = remaining;
    }

    for (uint32_t b = 0; b < blocks_in_group; b++) {
      uint32_t byte_idx = b / 8;
      uint8_t bit_mask = 1 << (b % 8);
      if (!(bitmap[byte_idx] & bit_mask)) {
        // Found a free block — mark it used
        bitmap[byte_idx] |= bit_mask;
        ext3_journal_block(mnt, mnt->bgdt[g].bg_block_bitmap, bitmap);

        mnt->bgdt[g].bg_free_blocks_count--;
        mnt->sb.s_free_blocks_count--;
        ext2_write_bgdt(mnt);
        ext2_write_superblock(mnt);

        ext3_journal_stop(mnt);
        kfree(bitmap);
        return g * mnt->sb.s_blocks_per_group + b + mnt->sb.s_first_data_block;
      }
    }
  }

  ext3_journal_stop(mnt);
  kfree(bitmap);
  return 0; // No free blocks
}

// Allocate a free inode. Returns inode number or 0 on failure.
static uint32_t ext2_alloc_inode(ext2_mount_t *mnt) {
  ext3_journal_start(mnt);
  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap)
    return 0;

  for (uint32_t g = 0; g < mnt->groups_count; g++) {
    if (mnt->bgdt[g].bg_free_inodes_count == 0)
      continue;

    ext2_read_block(mnt, mnt->bgdt[g].bg_inode_bitmap, bitmap);

    for (uint32_t i = 0; i < mnt->inodes_per_group; i++) {
      uint32_t byte_idx = i / 8;
      uint8_t bit_mask = 1 << (i % 8);
      if (!(bitmap[byte_idx] & bit_mask)) {
        bitmap[byte_idx] |= bit_mask;
        ext3_journal_block(mnt, mnt->bgdt[g].bg_inode_bitmap, bitmap);

        mnt->bgdt[g].bg_free_inodes_count--;
        mnt->sb.s_free_inodes_count--;
        ext2_write_bgdt(mnt);
        ext2_write_superblock(mnt);

        ext3_journal_stop(mnt);
        kfree(bitmap);
        return g * mnt->inodes_per_group + i + 1; // Inodes are 1-indexed
      }
    }
  }

  ext3_journal_stop(mnt);
  kfree(bitmap);
  return 0;
}

// ── Block / Inode Deallocation ──────────────────────────────────────────────

// Free a previously allocated block. Returns 0 on success.
static int ext2_free_block(ext2_mount_t *mnt, uint32_t block_num) {
  if (block_num == 0)
    return -1;

  uint32_t adjusted = block_num - mnt->sb.s_first_data_block;
  uint32_t group = adjusted / mnt->sb.s_blocks_per_group;
  uint32_t index = adjusted % mnt->sb.s_blocks_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap)
    return -1;

  ext2_read_block(mnt, mnt->bgdt[group].bg_block_bitmap, bitmap);

  uint32_t byte_idx = index / 8;
  uint8_t bit_mask = 1 << (index % 8);
  bitmap[byte_idx] &= ~bit_mask; // Clear the bit

  ext3_journal_block(mnt, mnt->bgdt[group].bg_block_bitmap, bitmap);
  kfree(bitmap);

  mnt->bgdt[group].bg_free_blocks_count++;
  mnt->sb.s_free_blocks_count++;
  ext2_write_bgdt(mnt);
  ext2_write_superblock(mnt);
  return 0;
}

// Free a previously allocated inode. Returns 0 on success.
static int ext2_free_inode(ext2_mount_t *mnt, uint32_t inode_num) {
  if (inode_num == 0)
    return -1;

  uint32_t group = (inode_num - 1) / mnt->inodes_per_group;
  uint32_t index = (inode_num - 1) % mnt->inodes_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap)
    return -1;

  ext2_read_block(mnt, mnt->bgdt[group].bg_inode_bitmap, bitmap);

  uint32_t byte_idx = index / 8;
  uint8_t bit_mask = 1 << (index % 8);
  bitmap[byte_idx] &= ~bit_mask; // Clear the bit

  ext3_journal_block(mnt, mnt->bgdt[group].bg_inode_bitmap, bitmap);
  kfree(bitmap);

  mnt->bgdt[group].bg_free_inodes_count++;
  mnt->sb.s_free_inodes_count++;
  ext2_write_bgdt(mnt);
  ext2_write_superblock(mnt);
  return 0;
}

// ── File data block resolution ──────────────────────────────────────────────

// Get the disk block number for a given logical block index in an inode.
// Supports direct, singly-indirect, and doubly-indirect blocks.
uint32_t ext2_get_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                            uint32_t logical_block) {
  uint32_t ptrs_per_block = mnt->block_size / 4;

  // Direct blocks (0..11)
  if (logical_block < EXT2_DIRECT_BLOCKS) {
    return inode->i_block[logical_block];
  }

  logical_block -= EXT2_DIRECT_BLOCKS;

  // Singly indirect (12..12+ptrs_per_block-1)
  if (logical_block < ptrs_per_block) {
    if (inode->i_block[12] == 0)
      return 0;
    uint32_t *indirect = kmalloc(mnt->block_size);
    if (!indirect)
      return 0;
    ext2_read_block(mnt, inode->i_block[12], indirect);
    uint32_t result = indirect[logical_block];
    kfree(indirect);
    return result;
  }

  logical_block -= ptrs_per_block;

  // Doubly indirect
  if (logical_block < ptrs_per_block * ptrs_per_block) {
    if (inode->i_block[13] == 0)
      return 0;
    uint32_t *dindirect = kmalloc(mnt->block_size);
    if (!dindirect)
      return 0;
    ext2_read_block(mnt, inode->i_block[13], dindirect);
    uint32_t indirect_block = dindirect[logical_block / ptrs_per_block];
    kfree(dindirect);
    if (indirect_block == 0)
      return 0;
    uint32_t *indirect = kmalloc(mnt->block_size);
    if (!indirect)
      return 0;
    ext2_read_block(mnt, indirect_block, indirect);
    uint32_t result = indirect[logical_block % ptrs_per_block];
    kfree(indirect);
    return result;
  }

  logical_block -= ptrs_per_block * ptrs_per_block;

  // Triply indirect
  if (logical_block <
      (uint64_t)ptrs_per_block * ptrs_per_block * ptrs_per_block) {
    if (inode->i_block[14] == 0)
      return 0;
    uint32_t *tindirect = kmalloc(mnt->block_size);
    if (!tindirect)
      return 0;
    ext2_read_block(mnt, inode->i_block[14], tindirect);

    uint32_t idx1 = logical_block / (ptrs_per_block * ptrs_per_block);
    uint32_t rem = logical_block % (ptrs_per_block * ptrs_per_block);
    uint32_t dindirect_block = tindirect[idx1];
    kfree(tindirect);
    if (dindirect_block == 0)
      return 0;

    uint32_t *dindirect = kmalloc(mnt->block_size);
    if (!dindirect)
      return 0;
    ext2_read_block(mnt, dindirect_block, dindirect);

    uint32_t idx2 = rem / ptrs_per_block;
    uint32_t indirect_block = dindirect[idx2];
    kfree(dindirect);
    if (indirect_block == 0)
      return 0;

    uint32_t *indirect = kmalloc(mnt->block_size);
    if (!indirect)
      return 0;
    ext2_read_block(mnt, indirect_block, indirect);
    uint32_t result = indirect[rem % ptrs_per_block];
    kfree(indirect);
    return result;
  }

  return 0; // Beyond addressable range
}

// Set a disk block number for a given logical block index, allocating indirect
// blocks as needed. Returns 0 on success, -1 on failure.
static int ext2_set_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                              uint32_t logical_block, uint32_t disk_block) {
  uint32_t ptrs_per_block = mnt->block_size / 4;

  // Direct blocks
  if (logical_block < EXT2_DIRECT_BLOCKS) {
    inode->i_block[logical_block] = disk_block;
    return 0;
  }

  logical_block -= EXT2_DIRECT_BLOCKS;

  // Singly indirect
  if (logical_block < ptrs_per_block) {
    if (inode->i_block[12] == 0) {
      uint32_t new_block = ext2_alloc_block(mnt);
      if (!new_block)
        return -1;
      inode->i_block[12] = new_block;
      // Zero out the new indirect block
      uint8_t *zero = kcalloc(1, mnt->block_size);
      ext2_write_block(mnt, new_block, zero);
      kfree(zero);
    }
    uint32_t *indirect = kmalloc(mnt->block_size);
    if (!indirect)
      return -1;
    ext2_read_block(mnt, inode->i_block[12], indirect);
    indirect[logical_block] = disk_block;
    ext2_write_block(mnt, inode->i_block[12], indirect);
    kfree(indirect);
    return 0;
  }

  logical_block -= ptrs_per_block;

  // Doubly indirect
  if (logical_block < ptrs_per_block * ptrs_per_block) {
    if (inode->i_block[13] == 0) {
      uint32_t new_block = ext2_alloc_block(mnt);
      if (!new_block)
        return -1;
      inode->i_block[13] = new_block;
      uint8_t *zero = kcalloc(1, mnt->block_size);
      ext2_write_block(mnt, new_block, zero);
      kfree(zero);
    }
    uint32_t *dindirect = kmalloc(mnt->block_size);
    if (!dindirect)
      return -1;
    ext2_read_block(mnt, inode->i_block[13], dindirect);

    uint32_t idx1 = logical_block / ptrs_per_block;
    uint32_t idx2 = logical_block % ptrs_per_block;

    if (dindirect[idx1] == 0) {
      uint32_t new_block = ext2_alloc_block(mnt);
      if (!new_block) {
        kfree(dindirect);
        return -1;
      }
      dindirect[idx1] = new_block;
      ext2_write_block(mnt, inode->i_block[13], dindirect);
      uint8_t *zero = kcalloc(1, mnt->block_size);
      ext2_write_block(mnt, new_block, zero);
      kfree(zero);
    }

    uint32_t *indirect = kmalloc(mnt->block_size);
    if (!indirect) {
      kfree(dindirect);
      return -1;
    }
    ext2_read_block(mnt, dindirect[idx1], indirect);
    indirect[idx2] = disk_block;
    ext2_write_block(mnt, dindirect[idx1], indirect);
    kfree(indirect);
    kfree(dindirect);
    return 0;
  }

  logical_block -= ptrs_per_block * ptrs_per_block;

  // Triply indirect
  if (logical_block <
      (uint64_t)ptrs_per_block * ptrs_per_block * ptrs_per_block) {
    // Allocate top-level triple indirect block if needed
    if (inode->i_block[14] == 0) {
      uint32_t new_block = ext2_alloc_block(mnt);
      if (!new_block)
        return -1;
      inode->i_block[14] = new_block;
      uint8_t *zero = kcalloc(1, mnt->block_size);
      ext2_write_block(mnt, new_block, zero);
      kfree(zero);
    }

    uint32_t *tindirect = kmalloc(mnt->block_size);
    if (!tindirect)
      return -1;
    ext2_read_block(mnt, inode->i_block[14], tindirect);

    uint32_t idx1 = logical_block / (ptrs_per_block * ptrs_per_block);
    uint32_t rem = logical_block % (ptrs_per_block * ptrs_per_block);

    // Allocate doubly-indirect block if needed
    if (tindirect[idx1] == 0) {
      uint32_t new_block = ext2_alloc_block(mnt);
      if (!new_block) {
        kfree(tindirect);
        return -1;
      }
      tindirect[idx1] = new_block;
      ext2_write_block(mnt, inode->i_block[14], tindirect);
      uint8_t *zero = kcalloc(1, mnt->block_size);
      ext2_write_block(mnt, new_block, zero);
      kfree(zero);
    }

    uint32_t *dindirect = kmalloc(mnt->block_size);
    if (!dindirect) {
      kfree(tindirect);
      return -1;
    }
    ext2_read_block(mnt, tindirect[idx1], dindirect);

    uint32_t idx2 = rem / ptrs_per_block;
    uint32_t idx3 = rem % ptrs_per_block;

    // Allocate singly-indirect block if needed
    if (dindirect[idx2] == 0) {
      uint32_t new_block = ext2_alloc_block(mnt);
      if (!new_block) {
        kfree(dindirect);
        kfree(tindirect);
        return -1;
      }
      dindirect[idx2] = new_block;
      ext2_write_block(mnt, tindirect[idx1], dindirect);
      uint8_t *zero = kcalloc(1, mnt->block_size);
      ext2_write_block(mnt, new_block, zero);
      kfree(zero);
    }

    uint32_t *indirect = kmalloc(mnt->block_size);
    if (!indirect) {
      kfree(dindirect);
      kfree(tindirect);
      return -1;
    }
    ext2_read_block(mnt, dindirect[idx2], indirect);
    indirect[idx3] = disk_block;
    ext2_write_block(mnt, dindirect[idx2], indirect);

    kfree(indirect);
    kfree(dindirect);
    kfree(tindirect);
    return 0;
  }

  return -1; // Beyond addressable range
}

// ── VFS Node Creation ───────────────────────────────────────────────────────

static vfs_node_t *ext2_make_vfs_node(ext2_mount_t *mnt, uint32_t inode_num,
                                      ext2_inode_t *inode) {
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;
  vfs_node_init(node);

  node->inode = inode_num;
  node->mask = inode->i_mode & 0x0FFF;
  node->uid = inode->i_uid;
  node->gid = inode->i_gid;
  node->length = inode->i_size;
  node->device = mnt; // Store mount context
  node->atime = inode->i_atime;
  node->mtime = inode->i_mtime;
  node->ctime = inode->i_ctime;

  if ((inode->i_mode & 0xF000) == EXT2_S_IFDIR) {
    node->flags = FS_DIRECTORY;
    node->readdir = ext2_readdir_impl;
    node->finddir = ext2_finddir_impl;
    node->create = ext2_create_impl;
    node->mkdir = ext2_mkdir_impl;
    node->unlink = ext2_unlink_impl;
    node->rmdir = ext2_rmdir_impl;
    node->symlink = ext2_symlink_impl;
    node->rename = ext2_rename_impl;
    node->chmod = ext2_chmod_impl;
    node->chown = ext2_chown_impl;
    node->mknod = ext2_mknod_impl;
  } else if ((inode->i_mode & 0xF000) == EXT2_S_IFREG) {
    node->flags = FS_FILE;
    node->read = ext2_read_impl;
    node->write = ext2_write_impl;
    node->truncate = ext2_truncate_impl;
    node->mmap = ext2_mmap_impl;
    node->chmod = ext2_chmod_impl;
    node->chown = ext2_chown_impl;
  } else if ((inode->i_mode & 0xF000) == EXT2_S_IFLNK) {
    node->flags = FS_SYMLINK;
    node->readlink = ext2_readlink_impl;
    node->chmod = ext2_chmod_impl;
    node->chown = ext2_chown_impl;
  } else if ((inode->i_mode & 0xF000) == EXT2_S_IFSOCK) {
    node->flags = FS_SOCKET;
    node->chmod = ext2_chmod_impl;
    node->chown = ext2_chown_impl;
    // We don't set read/write/poll here because rendezvous sockets are distinct
    // from data sockets
  }

  return node;
}

// ── VFS Read Implementation ─────────────────────────────────────────────────

static uint32_t ext2_read_impl(vfs_node_t *node, uint32_t offset, uint32_t size,
                               uint8_t *buffer) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return 0;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return 0;

  if (offset >= inode.i_size)
    return 0;
  if (offset + size > inode.i_size) {
    size = inode.i_size - offset;
  }

  uint32_t bytes_read = 0;
  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return 0;

  while (bytes_read < size) {
    uint32_t current_offset = offset + bytes_read;
    uint32_t logical_block = current_offset / mnt->block_size;
    uint32_t offset_in_block = current_offset % mnt->block_size;
    uint32_t to_copy = mnt->block_size - offset_in_block;
    if (to_copy > size - bytes_read) {
      to_copy = size - bytes_read;
    }

    uint32_t disk_block = ext2_get_block_num(mnt, &inode, logical_block);
    if (disk_block == 0) {
      memset(buffer + bytes_read, 0, to_copy);
    } else {
      ext2_read_block(mnt, disk_block, block_buf);
      memcpy(buffer + bytes_read, block_buf + offset_in_block, to_copy);
    }

    bytes_read += to_copy;
  }

  kfree(block_buf);
  return bytes_read;
}

static int ext2_truncate_impl(vfs_node_t *node, uint32_t new_len) {
  if (!node || node->flags != FS_FILE || !node->device)
    return -1;

  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode) != 0)
    return -1;

  inode.i_size = new_len;
  node->length = new_len;

  // We just update the size and rewrite the inode.
  // Blocks are intentionally not freed here for simplicity,
  // they will be naturally reused if the file grows again.
  return ext2_write_inode(mnt, node->inode, &inode);
}

static uint32_t ext2_write_impl(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return 0;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return 0;

  uint32_t bytes_written = 0;
  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return 0;

  while (bytes_written < size) {
    uint32_t current_offset = offset + bytes_written;
    uint32_t logical_block = current_offset / mnt->block_size;
    uint32_t offset_in_block = current_offset % mnt->block_size;
    uint32_t to_write = mnt->block_size - offset_in_block;
    if (to_write > size - bytes_written) {
      to_write = size - bytes_written;
    }

    uint32_t disk_block = ext2_get_block_num(mnt, &inode, logical_block);

    // Allocate a new block if needed
    if (disk_block == 0) {
      disk_block = ext2_alloc_block(mnt);
      if (disk_block == 0)
        break; // Out of space
      ext2_set_block_num(mnt, &inode, logical_block, disk_block);
      inode.i_blocks += mnt->block_size / 512;
      // Zero the new block first
      memset(block_buf, 0, mnt->block_size);
    } else if (offset_in_block != 0 || to_write < mnt->block_size) {
      // Partial block write — read existing data first
      ext2_read_block(mnt, disk_block, block_buf);
    }

    memcpy(block_buf + offset_in_block, buffer + bytes_written, to_write);
    ext2_write_block(mnt, disk_block, block_buf);

    bytes_written += to_write;
  }

  // Update size if we wrote past the end
  if (offset + bytes_written > inode.i_size) {
    inode.i_size = offset + bytes_written;
  }

  // Update modification timestamp
  uint32_t now = ext2_current_time();
  inode.i_mtime = now;
  inode.i_ctime = now;

  ext3_journal_start(mnt);
  ext2_write_inode(mnt, node->inode, &inode);
  ext3_journal_stop(mnt);
  node->length = inode.i_size;

  kfree(block_buf);
  return bytes_written;
}

// ── Directory Operations ────────────────────────────────────────────────────

static struct dirent *ext2_readdir_impl(vfs_node_t *node, uint32_t index) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return NULL;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return NULL;

  static struct dirent d;
  memset(&d, 0, sizeof(d));

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return NULL;

  uint32_t dir_size = inode.i_size;
  uint32_t byte_pos = 0;
  uint32_t entry_index = 0;

  while (byte_pos < dir_size) {
    uint32_t logical_block = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (offset_in_block == 0) {
      uint32_t disk_block = ext2_get_block_num(mnt, &inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);

    if (entry->inode != 0 && entry->rec_len > 0) {
      if (entry_index == index) {
        uint32_t name_len = entry->name_len;
        if (name_len > 127)
          name_len = 127;
        memcpy(d.name, entry->name, name_len);
        d.name[name_len] = '\0';
        d.ino = entry->inode;
        kfree(block_buf);
        return &d;
      }
      entry_index++;
    }

    if (entry->rec_len == 0)
      break; // Malformed
    byte_pos += entry->rec_len;
  }

  kfree(block_buf);
  return NULL;
}

static vfs_node_t *ext2_finddir_impl(vfs_node_t *node, char *name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return NULL;

  ext2_inode_t dir_inode;
  if (ext2_read_inode(mnt, node->inode, &dir_inode))
    return NULL;

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return NULL;

  uint32_t dir_size = dir_inode.i_size;
  uint32_t byte_pos = 0;
  uint32_t name_len = strlen(name);

  while (byte_pos < dir_size) {
    uint32_t logical_block = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (offset_in_block == 0) {
      uint32_t disk_block = ext2_get_block_num(mnt, &dir_inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);

    if (entry->inode != 0 && entry->name_len == name_len) {
      // Compare name (entry->name is not null-terminated on disk)
      bool match = true;
      for (uint32_t i = 0; i < name_len; i++) {
        if (entry->name[i] != name[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        uint32_t found_ino = entry->inode;
        kfree(block_buf);

        // Read the target inode and create a VFS node
        ext2_inode_t target_inode;
        if (ext2_read_inode(mnt, found_ino, &target_inode))
          return NULL;

        vfs_node_t *result = ext2_make_vfs_node(mnt, found_ino, &target_inode);
        if (result) {
          // Copy the name for display
          if (name_len > 127)
            name_len = 127;
          memcpy(result->name, name, name_len);
          result->name[name_len] = '\0';
        }
        return result;
      }
    }

    if (entry->rec_len == 0)
      break;
    byte_pos += entry->rec_len;
  }

  kfree(block_buf);
  return NULL;
}

// ── Write: Add a directory entry ────────────────────────────────────────────

// Add a new directory entry to a directory inode.
static int ext2_add_dir_entry(ext2_mount_t *mnt, uint32_t dir_inode_num,
                              uint32_t child_inode_num, const char *name,
                              uint8_t file_type) {
  ext2_inode_t dir_inode;
  if (ext2_read_inode(mnt, dir_inode_num, &dir_inode))
    return -1;

  uint32_t name_len = strlen(name);
  // Minimum entry size: 8 bytes header + name, rounded up to 4-byte boundary
  uint32_t needed = ((8 + name_len) + 3) & ~3;

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return -1;

  uint32_t dir_size = dir_inode.i_size;
  uint32_t byte_pos = 0;

  // Try to find space in existing directory blocks
  while (byte_pos < dir_size) {
    uint32_t logical_block = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (offset_in_block == 0) {
      uint32_t disk_block = ext2_get_block_num(mnt, &dir_inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);
    if (entry->rec_len == 0)
      break;

    // Calculate real size of this entry
    uint32_t real_size = ((8 + entry->name_len) + 3) & ~3;
    uint32_t slack = entry->rec_len - real_size;

    if (slack >= needed) {
      // Enough room — split this entry
      uint32_t old_rec_len = entry->rec_len;
      entry->rec_len = (uint16_t)real_size;

      // New entry lives right after
      ext2_dirent_t *new_entry =
          (ext2_dirent_t *)(block_buf + offset_in_block + real_size);
      new_entry->inode = child_inode_num;
      new_entry->rec_len = (uint16_t)(old_rec_len - real_size);
      new_entry->name_len = (uint8_t)name_len;
      new_entry->file_type = file_type;
      memcpy(new_entry->name, name, name_len);

      uint32_t disk_block =
          ext2_get_block_num(mnt, &dir_inode, byte_pos / mnt->block_size);
      ext2_write_block(mnt, disk_block, block_buf);
      kfree(block_buf);
      return 0;
    }

    byte_pos += entry->rec_len;
  }

  // No space in existing blocks — allocate a new directory block
  uint32_t new_block = ext2_alloc_block(mnt);
  if (!new_block) {
    kfree(block_buf);
    return -1;
  }

  uint32_t logical_block = dir_size / mnt->block_size;
  ext2_set_block_num(mnt, &dir_inode, logical_block, new_block);
  dir_inode.i_size += mnt->block_size;
  dir_inode.i_blocks += mnt->block_size / 512;

  // Fill the new block with our entry
  memset(block_buf, 0, mnt->block_size);
  ext2_dirent_t *new_entry = (ext2_dirent_t *)block_buf;
  new_entry->inode = child_inode_num;
  new_entry->rec_len = (uint16_t)mnt->block_size; // Takes up entire block
  new_entry->name_len = (uint8_t)name_len;
  new_entry->file_type = file_type;
  memcpy(new_entry->name, name, name_len);

  ext2_write_block(mnt, new_block, block_buf);
  ext2_write_inode(mnt, dir_inode_num, &dir_inode);

  kfree(block_buf);
  return 0;
}

// ── VFS create (new file) ───────────────────────────────────────────────────

static int ext2_create_impl(vfs_node_t *node, char *name, uint16_t permission) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  // Don't create if it already exists
  if (ext2_finddir_impl(node, name) != NULL)
    return -1;

  // Allocate a new inode
  uint32_t new_ino = ext2_alloc_inode(mnt);
  if (!new_ino)
    return -1;

  // Initialize the new inode
  ext2_inode_t new_inode;
  memset(&new_inode, 0, sizeof(ext2_inode_t));
  new_inode.i_mode = EXT2_S_IFREG | (permission & 0x0FFF);
  new_inode.i_size = 0;
  new_inode.i_links_count = 1;
  new_inode.i_blocks = 0;

  // Set timestamps
  uint32_t now = ext2_current_time();
  new_inode.i_atime = now;
  new_inode.i_ctime = now;
  new_inode.i_mtime = now;

  if (ext2_write_inode(mnt, new_ino, &new_inode))
    return -1;

  // Add directory entry
  if (ext2_add_dir_entry(mnt, node->inode, new_ino, name, EXT2_FT_REG_FILE))
    return -1;

  return 0;
}

// ── VFS mkdir (new directory) ───────────────────────────────────────────────

static int ext2_mkdir_impl(vfs_node_t *node, char *name, uint16_t permission) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  if (ext2_finddir_impl(node, name) != NULL)
    return -1;

  uint32_t new_ino = ext2_alloc_inode(mnt);
  if (!new_ino)
    return -1;

  // Allocate a data block for . and .. entries
  uint32_t data_block = ext2_alloc_block(mnt);
  if (!data_block)
    return -1;

  // Initialize the inode
  ext2_inode_t new_inode;
  memset(&new_inode, 0, sizeof(ext2_inode_t));
  new_inode.i_mode = EXT2_S_IFDIR | (permission & 0x0FFF);
  new_inode.i_size = mnt->block_size;
  new_inode.i_links_count = 2; // . and parent's entry
  new_inode.i_blocks = mnt->block_size / 512;
  new_inode.i_block[0] = data_block;

  // Set timestamps
  uint32_t now = ext2_current_time();
  new_inode.i_atime = now;
  new_inode.i_ctime = now;
  new_inode.i_mtime = now;

  // Build directory block with . and .. entries
  uint8_t *block_buf = kcalloc(1, mnt->block_size);
  if (!block_buf)
    return -1;

  // "." entry
  ext2_dirent_t *dot = (ext2_dirent_t *)block_buf;
  dot->inode = new_ino;
  dot->rec_len = 12; // 8 + 1 name byte, rounded to 4
  dot->name_len = 1;
  dot->file_type = EXT2_FT_DIR;
  dot->name[0] = '.';

  // ".." entry — takes up the rest of the block
  ext2_dirent_t *dotdot = (ext2_dirent_t *)(block_buf + 12);
  dotdot->inode = node->inode;
  dotdot->rec_len = (uint16_t)(mnt->block_size - 12);
  dotdot->name_len = 2;
  dotdot->file_type = EXT2_FT_DIR;
  dotdot->name[0] = '.';
  dotdot->name[1] = '.';

  ext2_write_block(mnt, data_block, block_buf);
  kfree(block_buf);

  if (ext2_write_inode(mnt, new_ino, &new_inode))
    return -1;

  // Add directory entry in parent
  if (ext2_add_dir_entry(mnt, node->inode, new_ino, name, EXT2_FT_DIR))
    return -1;

  // Increment parent's link count
  ext2_inode_t parent_inode;
  if (ext2_read_inode(mnt, node->inode, &parent_inode) == 0) {
    parent_inode.i_links_count++;
    ext2_write_inode(mnt, node->inode, &parent_inode);
  }

  // Update BGD used_dirs_count
  uint32_t group = (new_ino - 1) / mnt->inodes_per_group;
  mnt->bgdt[group].bg_used_dirs_count++;
  ext2_write_bgdt(mnt);

  return 0;
}

// ── Symlink Operations ──────────────────────────────────────────────────────

// Read the target path of a symbolic link.
// Fast symlinks (<=60 bytes) store the path directly in i_block[].
// Longer symlinks store the path in a data block.
static int ext2_readlink_impl(vfs_node_t *node, char *buf, uint32_t size) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return -1;

  if ((inode.i_mode & 0xF000) != EXT2_S_IFLNK)
    return -1;

  uint32_t link_len = inode.i_size;
  if (link_len == 0)
    return -1;

  // Fast symlink: target stored directly in i_block (up to 60 bytes)
  // Indicated by i_blocks == 0 (no data blocks allocated)
  if (inode.i_blocks == 0 && link_len <= 60) {
    uint32_t copy_len = (link_len < size - 1) ? link_len : size - 1;
    memcpy(buf, (const char *)inode.i_block, copy_len);
    buf[copy_len] = '\0';
    return (int)copy_len;
  }

  // Slow symlink: target stored in data block(s)
  uint32_t copy_len = (link_len < size - 1) ? link_len : size - 1;
  uint32_t bytes_read = ext2_read_impl(node, 0, copy_len, (uint8_t *)buf);
  buf[bytes_read] = '\0';
  return (int)bytes_read;
}

// Create a new symbolic link in a directory.
static int ext2_symlink_impl(vfs_node_t *node, char *name, char *target) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  // Don't create if it already exists
  if (ext2_finddir_impl(node, name) != NULL)
    return -1;

  // Allocate a new inode
  uint32_t new_ino = ext2_alloc_inode(mnt);
  if (!new_ino)
    return -1;

  uint32_t target_len = strlen(target);

  // Initialize the symlink inode
  ext2_inode_t new_inode;
  memset(&new_inode, 0, sizeof(ext2_inode_t));
  new_inode.i_mode =
      EXT2_S_IFLNK | 0777; // Symlinks always have 0777 permissions
  new_inode.i_size = target_len;
  new_inode.i_links_count = 1;

  // Set timestamps
  uint32_t now = ext2_current_time();
  new_inode.i_atime = now;
  new_inode.i_ctime = now;
  new_inode.i_mtime = now;

  // Fast symlink: store target directly in i_block if it fits (<=60 bytes)
  if (target_len <= 60) {
    memcpy((char *)new_inode.i_block, target, target_len);
    new_inode.i_blocks = 0; // No data blocks used
  } else {
    // Slow symlink: allocate a data block and write the target there
    uint32_t data_block = ext2_alloc_block(mnt);
    if (!data_block)
      return -1;

    new_inode.i_block[0] = data_block;
    new_inode.i_blocks = mnt->block_size / 512;

    uint8_t *block_buf = kcalloc(1, mnt->block_size);
    if (!block_buf)
      return -1;
    memcpy(block_buf, target, target_len);
    ext2_write_block(mnt, data_block, block_buf);
    kfree(block_buf);
  }

  if (ext2_write_inode(mnt, new_ino, &new_inode))
    return -1;

  // Add directory entry
  if (ext2_add_dir_entry(mnt, node->inode, new_ino, name, EXT2_FT_SYMLINK))
    return -1;

  return 0;
}

// ── Rename, Chmod, Chown ────────────────────────────────────────────────────

static int ext2_remove_dir_entry(ext2_mount_t *mnt, uint32_t dir_inode_num,
                                 const char *name);

static int ext2_rename_impl(vfs_node_t *node, char *old_name, char *new_name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  // Check if source exists and get its VFS node to find the target inode/type
  vfs_node_t *src_node = ext2_finddir_impl(node, old_name);
  if (!src_node)
    return -1;

  // Check if new_name already exists — POSIX requires overwrite for regular
  // files
  vfs_node_t *dst_node = ext2_finddir_impl(node, new_name);
  if (dst_node) {
    // Only allow overwriting regular files, not directories
    if ((dst_node->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
      kfree(dst_node);
      kfree(src_node);
      return -1; // Can't overwrite a directory
    }
    kfree(dst_node);
    // Unlink the existing target before renaming
    if (ext2_unlink_impl(node, new_name) != 0) {
      kfree(src_node);
      return -1;
    }
  }

  uint32_t target_inode_num = src_node->inode;
  uint32_t file_type = EXT2_FT_UNKNOWN;
  uint32_t ftype = src_node->flags & FS_TYPE_MASK;
  if (ftype == FS_FILE)
    file_type = EXT2_FT_REG_FILE;
  else if (ftype == FS_DIRECTORY)
    file_type = EXT2_FT_DIR;
  else if (ftype == FS_SYMLINK)
    file_type = EXT2_FT_SYMLINK;

  kfree(src_node); // We only needed the info

  // 1. Add the new directory entry
  if (ext2_add_dir_entry(mnt, node->inode, target_inode_num, new_name,
                         file_type))
    return -1;

  // 2. Remove the old directory entry
  if (ext2_remove_dir_entry(mnt, node->inode, old_name))
    return -1;

  // File rename complete (timestamps inside the moved inode are technically
  // unaffected by rename, but the parent directory's mtime/ctime will be
  // updated when we add support for that in directory ops)
  return 0;
}

static int ext2_chmod_impl(vfs_node_t *node, uint16_t permission) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return -1;

  // Preserve file type (upper 4 bits), replace permissions (lower 12 bits)
  inode.i_mode = (inode.i_mode & 0xF000) | (permission & 0x0FFF);
  inode.i_ctime = ext2_current_time();

  if (ext2_write_inode(mnt, node->inode, &inode))
    return -1;

  // Update the VFS node
  node->mask = permission & 0x0FFF;
  return 0;
}

static int ext2_chown_impl(vfs_node_t *node, uint32_t uid, uint32_t gid) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, node->inode, &inode))
    return -1;

  inode.i_uid = uid;
  inode.i_gid = gid;
  inode.i_ctime = ext2_current_time();

  if (ext2_write_inode(mnt, node->inode, &inode))
    return -1;

  // Update the VFS node
  node->uid = uid;
  node->gid = gid;
  return 0;
}

// ── Free all data blocks of an inode ────────────────────────────────────────

// Free a single indirect block and all data blocks it points to.
static void ext2_free_indirect(ext2_mount_t *mnt, uint32_t indirect_block) {
  if (indirect_block == 0)
    return;
  uint32_t *ptrs = kmalloc(mnt->block_size);
  if (!ptrs)
    return;
  ext2_read_block(mnt, indirect_block, ptrs);
  uint32_t ptrs_per_block = mnt->block_size / 4;
  for (uint32_t i = 0; i < ptrs_per_block; i++) {
    if (ptrs[i])
      ext2_free_block(mnt, ptrs[i]);
  }
  kfree(ptrs);
  ext2_free_block(mnt, indirect_block);
}

// Free a doubly-indirect block and everything underneath.
static void ext2_free_dindirect(ext2_mount_t *mnt, uint32_t dindirect_block) {
  if (dindirect_block == 0)
    return;
  uint32_t *ptrs = kmalloc(mnt->block_size);
  if (!ptrs)
    return;
  ext2_read_block(mnt, dindirect_block, ptrs);
  uint32_t ptrs_per_block = mnt->block_size / 4;
  for (uint32_t i = 0; i < ptrs_per_block; i++) {
    if (ptrs[i])
      ext2_free_indirect(mnt, ptrs[i]);
  }
  kfree(ptrs);
  ext2_free_block(mnt, dindirect_block);
}

// Free a triply-indirect block and everything underneath.
static void ext2_free_tindirect(ext2_mount_t *mnt, uint32_t tindirect_block) {
  if (tindirect_block == 0)
    return;
  uint32_t *ptrs = kmalloc(mnt->block_size);
  if (!ptrs)
    return;
  ext2_read_block(mnt, tindirect_block, ptrs);
  uint32_t ptrs_per_block = mnt->block_size / 4;
  for (uint32_t i = 0; i < ptrs_per_block; i++) {
    if (ptrs[i])
      ext2_free_dindirect(mnt, ptrs[i]);
  }
  kfree(ptrs);
  ext2_free_block(mnt, tindirect_block);
}

// Free ALL data blocks (direct + indirect + doubly + triply) of an inode.
static void ext2_free_all_blocks(ext2_mount_t *mnt, ext2_inode_t *inode) {
  // Direct blocks
  for (int i = 0; i < EXT2_DIRECT_BLOCKS; i++) {
    if (inode->i_block[i]) {
      ext2_free_block(mnt, inode->i_block[i]);
      inode->i_block[i] = 0;
    }
  }
  // Singly indirect
  if (inode->i_block[12]) {
    ext2_free_indirect(mnt, inode->i_block[12]);
    inode->i_block[12] = 0;
  }
  // Doubly indirect
  if (inode->i_block[13]) {
    ext2_free_dindirect(mnt, inode->i_block[13]);
    inode->i_block[13] = 0;
  }
  // Triply indirect
  if (inode->i_block[14]) {
    ext2_free_tindirect(mnt, inode->i_block[14]);
    inode->i_block[14] = 0;
  }
  inode->i_blocks = 0;
  inode->i_size = 0;
}

// ── Remove a directory entry by name ────────────────────────────────────────

static int ext2_remove_dir_entry(ext2_mount_t *mnt, uint32_t dir_inode_num,
                                 const char *name) {
  ext2_inode_t dir_inode;
  if (ext2_read_inode(mnt, dir_inode_num, &dir_inode))
    return -1;

  uint32_t name_len = strlen(name);
  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return -1;

  uint32_t dir_size = dir_inode.i_size;
  uint32_t byte_pos = 0;
  uint32_t prev_offset =
      0;                   // Byte offset of the previous entry in current block
  bool prev_valid = false; // Whether we have a valid previous entry
  uint32_t current_block_start =
      0; // Byte offset where the current block starts

  while (byte_pos < dir_size) {
    uint32_t logical_block = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (offset_in_block == 0) {
      uint32_t disk_block = ext2_get_block_num(mnt, &dir_inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
      prev_valid = false;
      current_block_start = byte_pos;
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);
    if (entry->rec_len == 0)
      break;

    if (entry->inode != 0 && entry->name_len == name_len) {
      bool match = true;
      for (uint32_t i = 0; i < name_len; i++) {
        if (entry->name[i] != name[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        if (prev_valid) {
          // Merge this entry's rec_len into the previous entry
          ext2_dirent_t *prev = (ext2_dirent_t *)(block_buf + prev_offset);
          prev->rec_len += entry->rec_len;
        } else {
          // First entry in block — just zero out the inode
          entry->inode = 0;
        }
        uint32_t disk_block = ext2_get_block_num(
            mnt, &dir_inode, current_block_start / mnt->block_size);
        ext2_write_block(mnt, disk_block, block_buf);
        kfree(block_buf);
        return 0;
      }
    }

    prev_offset = offset_in_block;
    prev_valid = (entry->inode != 0);
    byte_pos += entry->rec_len;
  }

  kfree(block_buf);
  return -1; // Not found
}

// ── Check if a directory is empty (only . and ..) ───────────────────────────

static bool ext2_dir_is_empty(ext2_mount_t *mnt, uint32_t inode_num) {
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, inode_num, &inode))
    return false;

  uint8_t *block_buf = kmalloc(mnt->block_size);
  if (!block_buf)
    return false;

  uint32_t dir_size = inode.i_size;
  uint32_t byte_pos = 0;

  while (byte_pos < dir_size) {
    uint32_t logical_block = byte_pos / mnt->block_size;
    uint32_t offset_in_block = byte_pos % mnt->block_size;

    if (offset_in_block == 0) {
      uint32_t disk_block = ext2_get_block_num(mnt, &inode, logical_block);
      if (disk_block == 0)
        break;
      ext2_read_block(mnt, disk_block, block_buf);
    }

    ext2_dirent_t *entry = (ext2_dirent_t *)(block_buf + offset_in_block);
    if (entry->rec_len == 0)
      break;

    if (entry->inode != 0) {
      // Skip "." and ".."
      bool is_dot = (entry->name_len == 1 && entry->name[0] == '.');
      bool is_dotdot = (entry->name_len == 2 && entry->name[0] == '.' &&
                        entry->name[1] == '.');
      if (!is_dot && !is_dotdot) {
        kfree(block_buf);
        return false; // Has real entries
      }
    }

    byte_pos += entry->rec_len;
  }

  kfree(block_buf);
  return true;
}

// ── VFS unlink (delete file) ────────────────────────────────────────────────

static int ext2_unlink_impl(vfs_node_t *node, char *name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  ext3_journal_start(mnt);

  // Find the target to get its inode number
  vfs_node_t *target = ext2_finddir_impl(node, name);
  if (!target)
    return -1;

  uint32_t target_ino = target->inode;
  kfree(target); // Free the VFS node we just created

  // Read the target inode
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, target_ino, &inode))
    return -1;

  // Don't unlink directories — use rmdir for that
  if ((inode.i_mode & 0xF000) == EXT2_S_IFDIR)
    return -1;

  // Remove the directory entry from the parent
  if (ext2_remove_dir_entry(mnt, node->inode, name))
    return -1;

  // Decrement link count
  inode.i_links_count--;

  if (inode.i_links_count == 0) {
    // No more links — free all data blocks and the inode itself
    ext2_free_all_blocks(mnt, &inode);
    inode.i_dtime = 1; // Mark as deleted (non-zero)
    ext2_write_inode(mnt, target_ino, &inode);
    ext2_free_inode(mnt, target_ino);
  } else {
    ext2_write_inode(mnt, target_ino, &inode);
  }

  ext3_journal_stop(mnt);
  return 0;
}

// ── VFS rmdir (delete empty directory) ──────────────────────────────────────

static int ext2_rmdir_impl(vfs_node_t *node, char *name) {
  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  if (!mnt)
    return -1;

  // Prevent removing . or ..
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return -1;

  // Find the target directory
  vfs_node_t *target = ext2_finddir_impl(node, name);
  if (!target)
    return -1;

  uint32_t target_ino = target->inode;
  kfree(target);

  ext2_inode_t inode;
  if (ext2_read_inode(mnt, target_ino, &inode))
    return -1;

  // Must be a directory
  if ((inode.i_mode & 0xF000) != EXT2_S_IFDIR)
    return -1;

  // Must be empty
  if (!ext2_dir_is_empty(mnt, target_ino))
    return -1;

  // Remove directory entry from parent
  if (ext2_remove_dir_entry(mnt, node->inode, name))
    return -1;

  // Free all data blocks and the inode
  ext2_free_all_blocks(mnt, &inode);
  inode.i_links_count = 0;
  inode.i_dtime = 1;
  ext2_write_inode(mnt, target_ino, &inode);
  ext2_free_inode(mnt, target_ino);

  // Decrement parent's link count (the ".." entry pointed to it)
  ext2_inode_t parent_inode;
  if (ext2_read_inode(mnt, node->inode, &parent_inode) == 0) {
    if (parent_inode.i_links_count > 0)
      parent_inode.i_links_count--;
    ext2_write_inode(mnt, node->inode, &parent_inode);
  }

  // Update BGD used_dirs_count
  uint32_t group = (target_ino - 1) / mnt->inodes_per_group;
  if (mnt->bgdt[group].bg_used_dirs_count > 0) {
    mnt->bgdt[group].bg_used_dirs_count--;
  }
  ext2_write_bgdt(mnt);

  return 0;
}

// ── Mount ───────────────────────────────────────────────────────────────────

int ext2_mount(struct block_device *dev, vfs_node_t *mountpoint) {
  if (!dev || !mountpoint)
    return -1;

  klog_puts("[EXT2] Probing block device '");
  klog_puts(dev->name);
  klog_puts("' for ext2 filesystem...  \n");

  // ── Read the superblock (byte offset 1024) ─────────────────────────
  uint8_t sb_buf[1024];
  // Read sectors 2 and 3 (byte offset 1024..2047)
  int err = dev->read_sectors(dev, 2, 2, sb_buf);
  if (err) {
    klog_puts("[EXT2] Failed to read superblock sectors.\n");
    return -1;
  }

  ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
  if (sb->s_magic != EXT2_MAGIC) {
    klog_puts("[EXT2] Invalid magic number. Not an ext2 filesystem.\n");
    return -1;
  }

  // ── Allocate and populate mount context ─────────────────────────────
  ext2_mount_t *mnt = kmalloc(sizeof(ext2_mount_t));
  if (!mnt)
    return -1;
  memset(mnt, 0, sizeof(ext2_mount_t));

  mnt->dev = dev;
  memcpy(&mnt->sb, sb, sizeof(ext2_superblock_t));
  mnt->block_size = 1024 << mnt->sb.s_log_block_size;
  mnt->inodes_per_group = mnt->sb.s_inodes_per_group;
  mnt->inode_size = (mnt->sb.s_rev_level >= 1) ? mnt->sb.s_inode_size
                                               : EXT2_GOOD_OLD_INODE_SIZE;
  mnt->groups_count =
      (mnt->sb.s_blocks_count + mnt->sb.s_blocks_per_group - 1) /
      mnt->sb.s_blocks_per_group;

  klog_puts("[EXT2] Superblock validated:\n");
  klog_puts("       Block size:  ");
  klog_uint64(mnt->block_size);
  klog_puts(" bytes\n");
  klog_puts("       Total blocks: ");
  klog_uint64(mnt->sb.s_blocks_count);
  klog_puts("\n");
  klog_puts("       Total inodes: ");
  klog_uint64(mnt->sb.s_inodes_count);
  klog_puts("\n");
  klog_puts("       Block groups: ");
  klog_uint64(mnt->groups_count);
  klog_puts("\n");
  klog_puts("       Inode size:   ");
  klog_uint64(mnt->inode_size);
  klog_puts(" bytes\n");

  // ── Read Block Group Descriptor Table ───────────────────────────────
  uint32_t bgdt_block = mnt->sb.s_first_data_block + 1;
  uint32_t bgdt_size = mnt->groups_count * sizeof(ext2_bgd_t);
  uint32_t bgdt_blocks = (bgdt_size + mnt->block_size - 1) / mnt->block_size;

  mnt->bgdt = kmalloc(bgdt_blocks * mnt->block_size);
  if (!mnt->bgdt) {
    kfree(mnt);
    return -1;
  }

  for (uint32_t i = 0; i < bgdt_blocks; i++) {
    err = ext2_read_block(mnt, bgdt_block + i,
                          (uint8_t *)mnt->bgdt + i * mnt->block_size);
    if (err) {
      klog_puts("[EXT2] Failed to read BGDT.\n");
      kfree(mnt->bgdt);
      kfree(mnt);
      return -1;
    }
  }

  klog_puts("[EXT2] Block Group Descriptor Table loaded.\n");

  // ── Read the root inode (always inode 2) ────────────────────────────
  ext2_inode_t root_inode;
  err = ext2_read_inode(mnt, EXT2_ROOT_INODE, &root_inode);
  if (err) {
    klog_puts("[EXT2] Failed to read root inode.\n");
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
    klog_puts("[EXT2] Root inode is not a directory!\n");
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  // ── Wire up the mountpoint ──────────────────────────────────────────
  vfs_node_t *root_vfs = ext2_make_vfs_node(mnt, EXT2_ROOT_INODE, &root_inode);
  if (!root_vfs) {
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  strcpy(root_vfs->name, "mnt");
  mnt->root_node = root_vfs;

  // Copy ext2 root's VFS callbacks onto the mountpoint
  mountpoint->flags = FS_DIRECTORY;
  mountpoint->inode = EXT2_ROOT_INODE;
  mountpoint->length = root_inode.i_size;
  mountpoint->device = mnt;
  mountpoint->readdir = ext2_readdir_impl;
  mountpoint->finddir = ext2_finddir_impl;
  mountpoint->create = ext2_create_impl;
  mountpoint->mkdir = ext2_mkdir_impl;
  mountpoint->unlink = ext2_unlink_impl;
  mountpoint->rmdir = ext2_rmdir_impl;
  mountpoint->symlink = ext2_symlink_impl;
  mountpoint->rename = ext2_rename_impl;
  mountpoint->chmod = ext2_chmod_impl;
  mountpoint->chown = ext2_chown_impl;

  ext3_init_journal(mnt);

  klog_puts("[OK] Ext2/3 filesystem mounted on /mnt\n");
  return 0;
}

// Mount ext2 as the root filesystem, replacing the current fs_root.
// This is used when we want ext2 to be the primary filesystem at /.
int ext2_mount_root(struct block_device *dev) {
  if (!dev)
    return -1;

  klog_puts("[EXT2] Probing block device '");
  klog_puts(dev->name);
  klog_puts("' for ext2 filesystem (as root)...\n");

  // Read the superblock (byte offset 1024)
  uint8_t sb_buf[1024];
  int err = dev->read_sectors(dev, 2, 2, sb_buf);
  if (err) {
    klog_puts("[EXT2] Failed to read superblock sectors.\n");
    return -1;
  }

  ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
  if (sb->s_magic != EXT2_MAGIC) {
    klog_puts("[EXT2] Invalid magic number. Not an ext2 filesystem.\n");
    return -1;
  }

  // Allocate and populate mount context
  ext2_mount_t *mnt = kmalloc(sizeof(ext2_mount_t));
  if (!mnt)
    return -1;
  memset(mnt, 0, sizeof(ext2_mount_t));

  mnt->dev = dev;
  memcpy(&mnt->sb, sb, sizeof(ext2_superblock_t));
  mnt->block_size = 1024 << mnt->sb.s_log_block_size;
  mnt->inodes_per_group = mnt->sb.s_inodes_per_group;
  mnt->inode_size = (mnt->sb.s_rev_level >= 1) ? mnt->sb.s_inode_size
                                               : EXT2_GOOD_OLD_INODE_SIZE;
  mnt->groups_count =
      (mnt->sb.s_blocks_count + mnt->sb.s_blocks_per_group - 1) /
      mnt->sb.s_blocks_per_group;

  klog_puts("[EXT2] Superblock validated:\n");
  klog_puts("       Block size:  ");
  klog_uint64(mnt->block_size);
  klog_puts(" bytes\n");
  klog_puts("       Total blocks: ");
  klog_uint64(mnt->sb.s_blocks_count);
  klog_puts("\n");
  klog_puts("       Total inodes: ");
  klog_uint64(mnt->sb.s_inodes_count);
  klog_puts("\n");
  klog_puts("       Block groups: ");
  klog_uint64(mnt->groups_count);
  klog_puts("\n");
  klog_puts("       Inode size:   ");
  klog_uint64(mnt->inode_size);
  klog_puts(" bytes\n");

  // Read Block Group Descriptor Table
  uint32_t bgdt_block = mnt->sb.s_first_data_block + 1;
  uint32_t bgdt_size = mnt->groups_count * sizeof(ext2_bgd_t);
  uint32_t bgdt_blocks = (bgdt_size + mnt->block_size - 1) / mnt->block_size;

  mnt->bgdt = kmalloc(bgdt_blocks * mnt->block_size);
  if (!mnt->bgdt) {
    kfree(mnt);
    return -1;
  }

  for (uint32_t i = 0; i < bgdt_blocks; i++) {
    err = ext2_read_block(mnt, bgdt_block + i,
                          (uint8_t *)mnt->bgdt + i * mnt->block_size);
    if (err) {
      klog_puts("[EXT2] Failed to read BGDT.\n");
      kfree(mnt->bgdt);
      kfree(mnt);
      return -1;
    }
  }

  klog_puts("[EXT2] Block Group Descriptor Table loaded.\n");

  // Read the root inode (always inode 2)
  ext2_inode_t root_inode;
  err = ext2_read_inode(mnt, EXT2_ROOT_INODE, &root_inode);
  if (err) {
    klog_puts("[EXT2] Failed to read root inode.\n");
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  if ((root_inode.i_mode & 0xF000) != EXT2_S_IFDIR) {
    klog_puts("[EXT2] Root inode is not a directory!\n");
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  // Create the VFS root node
  vfs_node_t *root_vfs = ext2_make_vfs_node(mnt, EXT2_ROOT_INODE, &root_inode);
  if (!root_vfs) {
    kfree(mnt->bgdt);
    kfree(mnt);
    return -1;
  }

  strcpy(root_vfs->name, "/");
  mnt->root_node = root_vfs;

  // Replace fs_root with the ext2 root
  fs_root = root_vfs;

  ext3_init_journal(mnt);

  klog_puts("[OK] Ext2/3 filesystem mounted as root (/)\n");
  return 0;
}

static int ext2_mknod_impl(vfs_node_t *node, char *name, uint16_t permission,
                           uint32_t flags, void *device) {
  if (flags != FS_SOCKET)
    return -1;

  int err = ext2_create_impl(node, name, permission);
  if (err < 0)
    return err;

  vfs_node_t *new_node = ext2_finddir_impl(node, name);
  if (!new_node)
    return -1;

  ext2_mount_t *mnt = (ext2_mount_t *)node->device;
  ext2_inode_t inode;
  if (ext2_read_inode(mnt, new_node->inode, &inode) != 0) {
    kfree(new_node);
    return -1;
  }

  inode.i_mode = (inode.i_mode & 0x0FFF) | EXT2_S_IFSOCK;
  ext2_write_inode(mnt, new_node->inode, &inode);

  new_node->flags = FS_SOCKET;
  new_node->device = device;
  kfree(new_node);
  return 0;
}
#define EXT2_MMAP_PROT_READ 0x1
#define EXT2_MMAP_PROT_WRITE 0x2
#define EXT2_MMAP_PROT_EXEC 0x4

#define EXT2_MAP_FIXED 0x10

static uint64_t ext2_mmap_impl(vfs_node_t *node, uint64_t addr, uint64_t length,
                               uint64_t prot, uint64_t flags, uint64_t offset) {
  if (length == 0)
    return (uint64_t)-1;
  if ((node->flags & FS_TYPE_MASK) != FS_FILE) {
    klog_puts("[EXT2_MMAP] Error: not a regular file\n");
    return (uint64_t)-1;
  }

  uint64_t aligned_len = (length + 4095) & ~4095ULL;
  uint64_t vaddr = addr;
  if (!(flags & EXT2_MAP_FIXED) || vaddr == 0) {
    vaddr = mm_alloc_mmap_region(aligned_len);
  }

  if (vaddr == 0) {
    klog_puts("[EXT2_MMAP] Error: mmap region exhausted\n");
    return (uint64_t)-1;
  }

  uint64_t num_pages = aligned_len / 4096;
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t hhdm = pmm_get_hhdm_offset();

  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & EXT2_MMAP_PROT_WRITE)
    page_flags |= PAGE_FLAG_RW;
  if (!(prot & EXT2_MMAP_PROT_EXEC))
    page_flags |= PAGE_FLAG_NX;

  for (uint64_t i = 0; i < num_pages; i++) {
    void *phys = pmm_alloc();
    if (!phys) {
      klog_puts("[EXT2_MMAP] Error: physical allocation failed\n");
      return (uint64_t)-1;
    }

    uint64_t virt_page = vaddr + i * 4096;
    uint64_t phys_page = (uint64_t)phys;

    // Zero the page first
    memset((void *)(phys_page + hhdm), 0, 4096);

    // Read from file into the physical page
    uint64_t file_off = offset + i * 4096;
    if (file_off < node->length) {
      uint32_t to_read = 4096;
      if (file_off + to_read > node->length) {
        to_read = node->length - file_off;
      }
      ext2_read_impl(node, (uint32_t)file_off, to_read,
                     (uint8_t *)(phys_page + hhdm));
    }

    if (!vmm_map_page(pml4, virt_page, phys_page, page_flags)) {
      klog_puts("[EXT2_MMAP] Error: vmm_map_page failed\n");
      return (uint64_t)-1;
    }
    vmm_flush_tlb(virt_page);
  }

  return vaddr;
}
