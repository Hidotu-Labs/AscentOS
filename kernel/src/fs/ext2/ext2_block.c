#include "ext2_internal.h"
#include "drivers/timer/rtc.h"
#include "fs/ext4/ext4_extent.h"

uint32_t ext2_current_time(void) {
  return (uint32_t)rtc_get_timestamp();
}

int ext2_read_block(ext2_mount_t *mnt, uint32_t block_num, void *buffer) {
  if (block_num == 0) {
    memset(buffer, 0, mnt->block_size);
    return 0;
  }

  for (int i = 0; i < 32; i++) {
    if (mnt->cache[i].data && mnt->cache[i].num == block_num) {
      memcpy(buffer, mnt->cache[i].data, mnt->block_size);
      return 0;
    }
  }

  uint64_t byte_offset = (uint64_t)block_num * mnt->block_size;
  uint64_t lba         = byte_offset / 512;
  uint32_t sectors     = mnt->block_size / 512;
  int err = mnt->dev->read_sectors(mnt->dev, lba, sectors, buffer);
  if (err)
    return err;

  int idx = block_num % 32;
  if (!mnt->cache[idx].data)
    mnt->cache[idx].data = kmalloc(mnt->block_size);
  if (mnt->cache[idx].data) {
    mnt->cache[idx].num = block_num;
    memcpy(mnt->cache[idx].data, buffer, mnt->block_size);
  }

  return 0;
}

int ext2_write_block(ext2_mount_t *mnt, uint32_t block_num,
                     const void *buffer) {
  if (block_num == 0)
    return -1;

  for (int i = 0; i < 32; i++) {
    if (mnt->cache[i].data && mnt->cache[i].num == block_num) {
      memcpy(mnt->cache[i].data, buffer, mnt->block_size);
      break;
    }
  }

  uint64_t byte_offset = (uint64_t)block_num * mnt->block_size;
  uint64_t lba         = byte_offset / 512;
  uint32_t sectors     = mnt->block_size / 512;
  return mnt->dev->write_sectors(mnt->dev, lba, sectors, buffer);
}

int ext2_write_superblock(ext2_mount_t *mnt) {
  uint8_t buf[1024];
  int err = mnt->dev->read_sectors(mnt->dev, 2, 2, buf);
  if (err)
    return -1;
  memcpy(buf, &mnt->sb, sizeof(ext2_superblock_t));
  return ext3_journal_block(mnt, 1, buf);
}

int ext2_write_bgdt(ext2_mount_t *mnt) {
  uint32_t bgdt_block    = mnt->sb.s_first_data_block + 1;
  uint32_t bgdt_size     = mnt->groups_count * sizeof(ext2_bgd_t);
  uint32_t blocks_needed = (bgdt_size + mnt->block_size - 1) / mnt->block_size;

  uint8_t *tmp = kmalloc(blocks_needed * mnt->block_size);
  if (!tmp)
    return -1;

  memset(tmp, 0, blocks_needed * mnt->block_size);
  memcpy(tmp, mnt->bgdt, bgdt_size);

  for (uint32_t i = 0; i < blocks_needed; i++) {
    int err = ext3_journal_block(mnt, bgdt_block + i,
                                 tmp + i * mnt->block_size);
    if (err) {
      kfree(tmp);
      return -1;
    }
  }

  kfree(tmp);
  return 0;
}

#define EXT2_INODE_STACK_BUF_MAX 4096

int ext2_read_inode(ext2_mount_t *mnt, uint32_t inode_num,
                    ext2_inode_t *out) {
  if (inode_num == 0)
    return -1;

  uint32_t group  = (inode_num - 1) / mnt->inodes_per_group;
  uint32_t index  = (inode_num - 1) % mnt->inodes_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint32_t inode_table_block    = mnt->bgdt[group].bg_inode_table;
  uint32_t byte_offset_in_table = index * mnt->inode_size;
  uint32_t block_offset         = byte_offset_in_table / mnt->block_size;
  uint32_t offset_within_block  = byte_offset_in_table % mnt->block_size;

  uint8_t stack_buf[EXT2_INODE_STACK_BUF_MAX];
  bool heap_used = (mnt->block_size > EXT2_INODE_STACK_BUF_MAX);
  uint8_t *block_buf = heap_used ? kmalloc(mnt->block_size) : stack_buf;
  if (!block_buf)
    return -1;

  int err = ext2_read_block(mnt, inode_table_block + block_offset, block_buf);
  if (err) {
    if (heap_used) kfree(block_buf);
    return -1;
  }

  memcpy(out, block_buf + offset_within_block, sizeof(ext2_inode_t));
  if (heap_used) kfree(block_buf);
  return 0;
}

int ext2_write_inode(ext2_mount_t *mnt, uint32_t inode_num,
                     const ext2_inode_t *inode) {
  if (inode_num == 0)
    return -1;

  uint32_t group  = (inode_num - 1) / mnt->inodes_per_group;
  uint32_t index  = (inode_num - 1) % mnt->inodes_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint32_t inode_table_block    = mnt->bgdt[group].bg_inode_table;
  uint32_t byte_offset_in_table = index * mnt->inode_size;
  uint32_t block_offset         = byte_offset_in_table / mnt->block_size;
  uint32_t offset_within_block  = byte_offset_in_table % mnt->block_size;

  uint8_t stack_buf[EXT2_INODE_STACK_BUF_MAX];
  bool heap_used = (mnt->block_size > EXT2_INODE_STACK_BUF_MAX);
  uint8_t *block_buf = heap_used ? kmalloc(mnt->block_size) : stack_buf;
  if (!block_buf)
    return -1;

  int err = ext2_read_block(mnt, inode_table_block + block_offset, block_buf);
  if (err) {
    if (heap_used) kfree(block_buf);
    return -1;
  }

  memcpy(block_buf + offset_within_block, inode, sizeof(ext2_inode_t));
  err = ext3_journal_block(mnt, inode_table_block + block_offset, block_buf);
  if (heap_used) kfree(block_buf);
  return err;
}

uint32_t ext2_get_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                             uint32_t logical_block) {
  if (inode->i_flags & EXT4_EXTENTS_FL)
    return ext4_get_block_num(mnt, inode, logical_block);
  uint32_t ptrs_per_block = mnt->block_size / 4;

  if (logical_block < EXT2_DIRECT_BLOCKS)
    return inode->i_block[logical_block];

  logical_block -= EXT2_DIRECT_BLOCKS;

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

  if (logical_block <
      (uint64_t)ptrs_per_block * ptrs_per_block * ptrs_per_block) {
    if (inode->i_block[14] == 0)
      return 0;
    uint32_t *tindirect = kmalloc(mnt->block_size);
    if (!tindirect)
      return 0;
    ext2_read_block(mnt, inode->i_block[14], tindirect);

    uint32_t idx1          = logical_block / (ptrs_per_block * ptrs_per_block);
    uint32_t rem           = logical_block % (ptrs_per_block * ptrs_per_block);
    uint32_t dindirect_blk = tindirect[idx1];
    kfree(tindirect);
    if (dindirect_blk == 0)
      return 0;

    uint32_t *dindirect = kmalloc(mnt->block_size);
    if (!dindirect)
      return 0;
    ext2_read_block(mnt, dindirect_blk, dindirect);

    uint32_t idx2           = rem / ptrs_per_block;
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

  return 0;
}

int ext2_set_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                       uint32_t logical_block, uint32_t disk_block) {
  uint32_t ptrs_per_block = mnt->block_size / 4;

  if (logical_block < EXT2_DIRECT_BLOCKS) {
    inode->i_block[logical_block] = disk_block;
    return 0;
  }

  logical_block -= EXT2_DIRECT_BLOCKS;

  if (logical_block < ptrs_per_block) {
    if (inode->i_block[12] == 0) {
      uint32_t new_block = ext2_alloc_block(mnt);
      if (!new_block)
        return -1;
      inode->i_block[12] = new_block;
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

  if (logical_block <
      (uint64_t)ptrs_per_block * ptrs_per_block * ptrs_per_block) {
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
    uint32_t rem  = logical_block % (ptrs_per_block * ptrs_per_block);

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

  return -1;
}

uint32_t ext2_alloc_block(ext2_mount_t *mnt) {
  ext3_journal_start(mnt);
  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap)
    return 0;

  for (uint32_t g = 0; g < mnt->groups_count; g++) {
    if (mnt->bgdt[g].bg_free_blocks_count == 0)
      continue;

    ext2_read_block(mnt, mnt->bgdt[g].bg_block_bitmap, bitmap);

    uint32_t blocks_in_group = mnt->sb.s_blocks_per_group;
    if (g == mnt->groups_count - 1) {
      uint32_t remaining =
          mnt->sb.s_blocks_count - (g * mnt->sb.s_blocks_per_group);
      if (remaining < blocks_in_group)
        blocks_in_group = remaining;
    }

    for (uint32_t b = 0; b < blocks_in_group; b++) {
      uint32_t byte_idx = b / 8;
      uint8_t  bit_mask = 1 << (b % 8);
      if (!(bitmap[byte_idx] & bit_mask)) {
        bitmap[byte_idx] |= bit_mask;
        ext3_journal_block(mnt, mnt->bgdt[g].bg_block_bitmap, bitmap);

        mnt->bgdt[g].bg_free_blocks_count--;
        mnt->sb.s_free_blocks_count--;
        ext2_write_bgdt(mnt);
        ext2_write_superblock(mnt);

        ext3_journal_stop(mnt);
        kfree(bitmap);
        return g * mnt->sb.s_blocks_per_group + b +
               mnt->sb.s_first_data_block;
      }
    }
  }

  ext3_journal_stop(mnt);
  kfree(bitmap);
  return 0;
}

uint32_t ext2_alloc_inode(ext2_mount_t *mnt) {
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
      uint8_t  bit_mask = 1 << (i % 8);
      if (!(bitmap[byte_idx] & bit_mask)) {
        bitmap[byte_idx] |= bit_mask;
        ext3_journal_block(mnt, mnt->bgdt[g].bg_inode_bitmap, bitmap);

        mnt->bgdt[g].bg_free_inodes_count--;
        mnt->sb.s_free_inodes_count--;
        ext2_write_bgdt(mnt);
        ext2_write_superblock(mnt);

        ext3_journal_stop(mnt);
        kfree(bitmap);
        return g * mnt->inodes_per_group + i + 1;
      }
    }
  }

  ext3_journal_stop(mnt);
  kfree(bitmap);
  return 0;
}

int ext2_free_block(ext2_mount_t *mnt, uint32_t block_num) {
  if (block_num == 0)
    return -1;

  uint32_t adjusted = block_num - mnt->sb.s_first_data_block;
  uint32_t group    = adjusted / mnt->sb.s_blocks_per_group;
  uint32_t index    = adjusted % mnt->sb.s_blocks_per_group;

  if (group >= mnt->groups_count)
    return -1;

  uint8_t *bitmap = kmalloc(mnt->block_size);
  if (!bitmap)
    return -1;

  ext2_read_block(mnt, mnt->bgdt[group].bg_block_bitmap, bitmap);

  uint32_t byte_idx = index / 8;
  uint8_t  bit_mask = 1 << (index % 8);
  bitmap[byte_idx] &= ~bit_mask;

  ext3_journal_block(mnt, mnt->bgdt[group].bg_block_bitmap, bitmap);
  kfree(bitmap);

  mnt->bgdt[group].bg_free_blocks_count++;
  mnt->sb.s_free_blocks_count++;
  ext2_write_bgdt(mnt);
  ext2_write_superblock(mnt);
  return 0;
}

int ext2_free_inode(ext2_mount_t *mnt, uint32_t inode_num) {
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
  uint8_t  bit_mask = 1 << (index % 8);
  bitmap[byte_idx] &= ~bit_mask;

  ext3_journal_block(mnt, mnt->bgdt[group].bg_inode_bitmap, bitmap);
  kfree(bitmap);

  mnt->bgdt[group].bg_free_inodes_count++;
  mnt->sb.s_free_inodes_count++;
  ext2_write_bgdt(mnt);
  ext2_write_superblock(mnt);
  return 0;
}

void ext2_free_indirect(ext2_mount_t *mnt, uint32_t indirect_block) {
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

void ext2_free_dindirect(ext2_mount_t *mnt, uint32_t dindirect_block) {
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

void ext2_free_tindirect(ext2_mount_t *mnt, uint32_t tindirect_block) {
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

void ext2_free_all_blocks(ext2_mount_t *mnt, ext2_inode_t *inode) {
  if (ext4_inode_has_extents(inode)) {
    ext4_extent_free_all(mnt, inode);
    return;
  }
  if ((inode->i_mode & 0xF000) == EXT2_S_IFLNK && inode->i_blocks == 0) {
    memset(inode->i_block, 0, sizeof(inode->i_block));
    inode->i_size = 0;
    return;
  }
  for (int i = 0; i < EXT2_DIRECT_BLOCKS; i++) {
    if (inode->i_block[i]) {
      ext2_free_block(mnt, inode->i_block[i]);
      inode->i_block[i] = 0;
    }
  }
  if (inode->i_block[12]) {
    ext2_free_indirect(mnt, inode->i_block[12]);
    inode->i_block[12] = 0;
  }
  if (inode->i_block[13]) {
    ext2_free_dindirect(mnt, inode->i_block[13]);
    inode->i_block[13] = 0;
  }
  if (inode->i_block[14]) {
    ext2_free_tindirect(mnt, inode->i_block[14]);
    inode->i_block[14] = 0;
  }
  inode->i_blocks = 0;
  inode->i_size   = 0;
}
