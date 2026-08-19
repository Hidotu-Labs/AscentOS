#include "ext4_extent.h"
#include "drivers/storage/block.h"
#include "fs/ext2/ext2_internal.h"
#include "console/klog.h"
#include "mm/heap.h"

uint64_t ext4_extent_get_pblock(ext4_extent_t *ex) {
    return ((uint64_t)ex->ee_start_hi << 32) | (uint64_t)ex->ee_start_lo;
}

bool ext4_inode_has_extents(ext2_inode_t *inode) {
    return (inode->i_flags & EXT4_EXTENTS_FL) != 0;
}

uint32_t ext4_get_block_num(ext2_mount_t *mnt, ext2_inode_t *inode, uint32_t logical_block) {
    uint8_t *bufs[5] = {NULL, NULL, NULL, NULL, NULL};
    int depth_idx = 0;
    uint32_t result = 0;

    ext4_extent_header_t *hdr = (ext4_extent_header_t *)inode->i_block;
    void *entries = (uint8_t *)inode->i_block + sizeof(ext4_extent_header_t);

    if (hdr->eh_magic != EXT4_EXT_MAGIC)
        goto done;

    while (1) {
        if (hdr->eh_depth == 0) {
            ext4_extent_t *exts = (ext4_extent_t *)entries;
            for (uint16_t i = 0; i < hdr->eh_entries; i++) {
                ext4_extent_t *ex = &exts[i];
                uint16_t len = ex->ee_len & 0x7FFF;
                if (logical_block >= ex->ee_block && logical_block < ex->ee_block + len) {
                    if (ex->ee_len > 0x8000)
                        goto done;
                    uint64_t pblock = ext4_extent_get_pblock(ex);
                    result = (uint32_t)(pblock + (logical_block - ex->ee_block));
                    goto done;
                }
            }
            goto done;
        } else {
            ext4_extent_idx_t *idxs = (ext4_extent_idx_t *)entries;
            ext4_extent_idx_t *best = NULL;
            for (uint16_t i = 0; i < hdr->eh_entries; i++) {
                if (idxs[i].ei_block <= logical_block)
                    best = &idxs[i];
            }
            if (!best || depth_idx >= 4)
                goto done;

            uint64_t child_phys = ((uint64_t)best->ei_leaf_hi << 32) | best->ei_leaf_lo;
            uint8_t *buf = kmalloc(mnt->block_size);
            if (!buf)
                goto done;
            bufs[depth_idx++] = buf;
            if (ext2_read_block(mnt, (uint32_t)child_phys, buf) != 0)
                goto done;

            hdr = (ext4_extent_header_t *)buf;
            entries = buf + sizeof(ext4_extent_header_t);
            if (hdr->eh_magic != EXT4_EXT_MAGIC)
                goto done;
        }
    }

done:
    for (int i = 0; i < 5; i++)
        if (bufs[i]) kfree(bufs[i]);
    return result;
}

void ext4_extent_init_inode(ext2_inode_t *inode) {
    memset(inode->i_block, 0, sizeof(inode->i_block));
    ext4_extent_header_t *hdr = (ext4_extent_header_t *)inode->i_block;
    hdr->eh_magic = EXT4_EXT_MAGIC;
    hdr->eh_max = 4;
    inode->i_flags |= EXT4_EXTENTS_FL;
}

static uint16_t ext4_extent_block_max(ext2_mount_t *mnt) {
    return (uint16_t)((mnt->block_size - sizeof(ext4_extent_header_t)) /
                      sizeof(ext4_extent_t));
}

static void ext4_extent_set_pblock(ext4_extent_t *ex, uint64_t phys_block) {
    ex->ee_start_lo = (uint32_t)phys_block;
    ex->ee_start_hi = (uint16_t)(phys_block >> 32);
}

static uint64_t ext4_idx_get_pblock(ext4_extent_idx_t *idx) {
    return ((uint64_t)idx->ei_leaf_hi << 32) | idx->ei_leaf_lo;
}

static void ext4_idx_set_pblock(ext4_extent_idx_t *idx, uint64_t phys_block) {
    idx->ei_leaf_lo = (uint32_t)phys_block;
    idx->ei_leaf_hi = (uint16_t)(phys_block >> 32);
}

static int ext4_extent_write_node(ext2_mount_t *mnt, uint32_t block,
                                  void *buffer) {
    /*
     * A newly grown extent leaf can be read again before the surrounding
     * inode update completes. Queueing it only in the journal left that leaf
     * unreadable to the next write; GNU ld exposes this with sparse output.
     * Persist the node now, matching the caller's direct inode update.
     */
    int result = ext2_write_block(mnt, block, buffer);
    if (result != 0)
        return result;

    /* Extent nodes are immediately traversed by later sparse writes.  Force
     * visibility before their small direct-mapped cache slot can be evicted. */
    return block_flush(mnt->dev);
}

static int ext4_extent_insert_leaf_entry(ext4_extent_header_t *hdr,
                                         ext4_extent_t *exts,
                                         uint32_t logical_block,
                                         uint64_t phys_block, uint16_t len) {
    uint16_t pos = 0;
    while (pos < hdr->eh_entries && exts[pos].ee_block < logical_block)
        pos++;

    if (pos > 0) {
        ext4_extent_t *left = &exts[pos - 1];
        uint16_t left_len = left->ee_len & 0x7FFF;
        uint64_t left_phys = ext4_extent_get_pblock(left);
        if (left->ee_block + left_len > logical_block)
            return -1;
        if (left->ee_block + left_len == logical_block &&
            left_phys + left_len == phys_block && left_len + len <= 0x7FFF) {
            left->ee_len = left_len + len;
            if (pos < hdr->eh_entries) {
                ext4_extent_t *right = &exts[pos];
                uint16_t right_len = right->ee_len & 0x7FFF;
                if (logical_block + len == right->ee_block &&
                    phys_block + len == ext4_extent_get_pblock(right) &&
                    left->ee_len + right_len <= 0x7FFF) {
                    left->ee_len += right_len;
                    for (uint16_t i = pos; i + 1 < hdr->eh_entries; i++)
                        exts[i] = exts[i + 1];
                    hdr->eh_entries--;
                }
            }
            return 0;
        }
    }

    if (pos < hdr->eh_entries) {
        ext4_extent_t *right = &exts[pos];
        if (logical_block + len > right->ee_block)
            return -1;
        if (logical_block + len == right->ee_block &&
            phys_block + len == ext4_extent_get_pblock(right) &&
            (right->ee_len & 0x7FFF) + len <= 0x7FFF) {
            right->ee_block = logical_block;
            right->ee_len = (right->ee_len & 0x7FFF) + len;
            ext4_extent_set_pblock(right, phys_block);
            return 0;
        }
    }

    if (hdr->eh_entries >= hdr->eh_max)
        return 1;

    for (uint16_t i = hdr->eh_entries; i > pos; i--)
        exts[i] = exts[i - 1];
    memset(&exts[pos], 0, sizeof(*exts));
    exts[pos].ee_block = logical_block;
    exts[pos].ee_len = len;
    ext4_extent_set_pblock(&exts[pos], phys_block);
    hdr->eh_entries++;
    return 0;
}

static int ext4_extent_grow_root(ext2_mount_t *mnt, ext2_inode_t *inode) {
    ext4_extent_header_t *root = (ext4_extent_header_t *)inode->i_block;
    uint32_t child_block = ext2_alloc_block(mnt);
    if (!child_block)
        return -1;

    uint8_t *child = kcalloc(1, mnt->block_size);
    if (!child) {
        ext2_free_block(mnt, child_block);
        return -1;
    }

    ext4_extent_header_t *child_hdr = (ext4_extent_header_t *)child;
    child_hdr->eh_magic = EXT4_EXT_MAGIC;
    child_hdr->eh_entries = root->eh_entries;
    child_hdr->eh_max = ext4_extent_block_max(mnt);
    child_hdr->eh_depth = root->eh_depth;
    child_hdr->eh_generation = root->eh_generation;
    memcpy(child + sizeof(*child_hdr),
           (uint8_t *)root + sizeof(*root),
           root->eh_entries * sizeof(ext4_extent_t));

    if (ext4_extent_write_node(mnt, child_block, child) != 0) {
        kfree(child);
        ext2_free_block(mnt, child_block);
        return -1;
    }

    uint32_t first_block = 0;
    if (child_hdr->eh_entries) {
        if (child_hdr->eh_depth == 0)
            first_block = ((ext4_extent_t *)(child + sizeof(*child_hdr)))->ee_block;
        else
            first_block = ((ext4_extent_idx_t *)(child + sizeof(*child_hdr)))->ei_block;
    }

    memset(inode->i_block, 0, sizeof(inode->i_block));
    root = (ext4_extent_header_t *)inode->i_block;
    root->eh_magic = EXT4_EXT_MAGIC;
    root->eh_entries = 1;
    root->eh_max = 4;
    root->eh_depth = child_hdr->eh_depth + 1;
    root->eh_generation = child_hdr->eh_generation;
    ext4_extent_idx_t *idx = (ext4_extent_idx_t *)(root + 1);
    idx->ei_block = first_block;
    ext4_idx_set_pblock(idx, child_block);
    inode->i_blocks += mnt->block_size / 512;
    kfree(child);
    return 0;
}

static int ext4_extent_add_root_index(ext2_inode_t *inode,
                                      uint32_t logical_block,
                                      uint32_t child_block) {
    ext4_extent_header_t *root = (ext4_extent_header_t *)inode->i_block;
    if (root->eh_entries >= root->eh_max)
        return -1;

    ext4_extent_idx_t *idxs = (ext4_extent_idx_t *)(root + 1);
    uint16_t pos = 0;
    while (pos < root->eh_entries && idxs[pos].ei_block < logical_block)
        pos++;
    for (uint16_t i = root->eh_entries; i > pos; i--)
        idxs[i] = idxs[i - 1];
    memset(&idxs[pos], 0, sizeof(*idxs));
    idxs[pos].ei_block = logical_block;
    ext4_idx_set_pblock(&idxs[pos], child_block);
    root->eh_entries++;
    return 0;
}

static int ext4_extent_split_leaf(ext2_mount_t *mnt, ext2_inode_t *inode,
                                  ext4_extent_header_t *leaf,
                                  uint32_t leaf_block, uint32_t logical_block,
                                  uint64_t phys_block, uint16_t len) {
    uint16_t total = leaf->eh_entries + 1;
    ext4_extent_t *all = kmalloc(total * sizeof(*all));
    if (!all)
        return -1;

    ext4_extent_t *old = (ext4_extent_t *)(leaf + 1);
    uint16_t pos = 0;
    while (pos < leaf->eh_entries && old[pos].ee_block < logical_block)
        pos++;
    memcpy(all, old, pos * sizeof(*all));
    memset(&all[pos], 0, sizeof(*all));
    all[pos].ee_block = logical_block;
    all[pos].ee_len = len;
    ext4_extent_set_pblock(&all[pos], phys_block);
    memcpy(&all[pos + 1], &old[pos],
           (leaf->eh_entries - pos) * sizeof(*all));

    uint32_t new_block = ext2_alloc_block(mnt);
    uint8_t *new_buf = new_block ? kcalloc(1, mnt->block_size) : NULL;
    if (!new_buf) {
        if (new_block)
            ext2_free_block(mnt, new_block);
        kfree(all);
        return -1;
    }

    uint16_t left_count = total / 2;
    uint16_t right_count = total - left_count;
    leaf->eh_entries = left_count;
    memcpy(old, all, left_count * sizeof(*old));

    ext4_extent_header_t *new_hdr = (ext4_extent_header_t *)new_buf;
    new_hdr->eh_magic = EXT4_EXT_MAGIC;
    new_hdr->eh_entries = right_count;
    new_hdr->eh_max = ext4_extent_block_max(mnt);
    new_hdr->eh_depth = 0;
    new_hdr->eh_generation = leaf->eh_generation;
    memcpy(new_hdr + 1, &all[left_count], right_count * sizeof(*all));

    int result = ext4_extent_write_node(mnt, leaf_block, leaf);
    if (result == 0)
        result = ext4_extent_write_node(mnt, new_block, new_buf);
    if (result == 0)
        result = ext4_extent_add_root_index(inode, all[left_count].ee_block,
                                            new_block);
    if (result == 0)
        inode->i_blocks += mnt->block_size / 512;
    else
        ext2_free_block(mnt, new_block);

    kfree(new_buf);
    kfree(all);
    return result;
}

int ext4_extent_insert(ext2_mount_t *mnt, ext2_inode_t *inode,
                       uint32_t inode_num, uint32_t logical_block,
                       uint64_t phys_block, uint16_t len) {
    (void)inode_num;
    if (!mnt || !inode || !len || len > 0x7FFF || phys_block > UINT32_MAX)
        return -1;
    if (!ext4_inode_has_extents(inode))
        ext4_extent_init_inode(inode);

retry:
    ext4_extent_header_t *root = (ext4_extent_header_t *)inode->i_block;
    if (root->eh_magic != EXT4_EXT_MAGIC || root->eh_depth > 1)
        return -1;

    if (root->eh_depth == 0) {
        int result = ext4_extent_insert_leaf_entry(
            root, (ext4_extent_t *)(root + 1), logical_block, phys_block, len);
        if (result != 1)
            return result;
        if (ext4_extent_grow_root(mnt, inode) != 0)
            return -1;
        goto retry;
    }

    ext4_extent_idx_t *idxs = (ext4_extent_idx_t *)(root + 1);
    ext4_extent_idx_t *best = NULL;
    for (uint16_t i = 0; i < root->eh_entries; i++) {
        if (idxs[i].ei_block <= logical_block)
            best = &idxs[i];
    }
    if (!best)
        best = &idxs[0];
    uint64_t leaf_phys = ext4_idx_get_pblock(best);
    if (!leaf_phys || leaf_phys > UINT32_MAX) {
        return -1;
    }

    uint8_t *buf = kmalloc(mnt->block_size);
    if (!buf) {
        return -1;
    }
    if (ext2_read_block(mnt, (uint32_t)leaf_phys, buf) != 0) {
        kfree(buf);
        return -1;
    }

    ext4_extent_header_t *leaf = (ext4_extent_header_t *)buf;
    if (leaf->eh_magic != EXT4_EXT_MAGIC || leaf->eh_depth != 0 ||
        leaf->eh_entries > leaf->eh_max ||
        leaf->eh_max > ext4_extent_block_max(mnt)) {
        klog_puts("[EXT4] invalid extent leaf block=");
        klog_uint64(leaf_phys);
        klog_puts(" magic=");
        klog_uint64(leaf->eh_magic);
        klog_puts(" depth=");
        klog_uint64(leaf->eh_depth);
        klog_puts(" entries=");
        klog_uint64(leaf->eh_entries);
        klog_puts(" max=");
        klog_uint64(leaf->eh_max);
        klog_puts("\n");
        kfree(buf);
        return -1;
    }

    int result = ext4_extent_insert_leaf_entry(
        leaf, (ext4_extent_t *)(leaf + 1), logical_block, phys_block, len);
    if (result == 0)
        result = ext4_extent_write_node(mnt, (uint32_t)leaf_phys, buf);
    else if (result == 1)
        result = ext4_extent_split_leaf(mnt, inode, leaf,
                                        (uint32_t)leaf_phys, logical_block,
                                        phys_block, len);
    kfree(buf);
    return result;
}

int ext4_alloc_extent(ext2_mount_t *mnt, ext2_inode_t *inode,
                      uint32_t inode_num, uint32_t logical_block,
                      uint32_t num_blocks, uint64_t *out_phys) {
    if (!mnt || !inode || !out_phys || !num_blocks || num_blocks > 0x7FFF)
        return -1;

    uint32_t *blocks = kmalloc(num_blocks * sizeof(*blocks));
    if (!blocks)
        return -1;

    if (ext3_journal_start(mnt) != 0) {
        kfree(blocks);
        return -1;
    }

    uint32_t count = 0;
    int result = -1;
    while (count < num_blocks) {
        blocks[count] = ext2_alloc_block(mnt);
        if (!blocks[count]) {
            goto done;
        }
        count++;
        if (count > 1 && blocks[count - 1] != blocks[0] + count - 1)
            goto done;
    }

    if (ext4_extent_insert(mnt, inode, inode_num, logical_block,
                           blocks[0], (uint16_t)num_blocks) != 0) {
        goto done;
    }

    *out_phys = blocks[0];
    result = 0;

done:
    if (result != 0) {
        for (uint32_t i = 0; i < count; i++)
            ext2_free_block(mnt, blocks[i]);
    }
    ext3_journal_stop(mnt);
    kfree(blocks);
    return result;
}
static void ext4_extent_unaccount(ext2_mount_t *mnt, ext2_inode_t *inode,
                                  uint32_t blocks) {
    uint32_t sectors = blocks * (mnt->block_size / 512);
    inode->i_blocks = inode->i_blocks > sectors ? inode->i_blocks - sectors : 0;
}

static uint32_t ext4_extent_first_logical(ext4_extent_header_t *hdr) {
    if (!hdr->eh_entries)
        return 0;
    if (hdr->eh_depth == 0)
        return ((ext4_extent_t *)(hdr + 1))[0].ee_block;
    return ((ext4_extent_idx_t *)(hdr + 1))[0].ei_block;
}

static int ext4_extent_free_node(ext2_mount_t *mnt,
                                 ext4_extent_header_t *hdr,
                                 ext2_inode_t *inode) {
    if (hdr->eh_magic != EXT4_EXT_MAGIC)
        return -1;

    if (hdr->eh_depth == 0) {
        ext4_extent_t *exts = (ext4_extent_t *)(hdr + 1);
        for (uint16_t i = 0; i < hdr->eh_entries; i++) {
            uint16_t len = exts[i].ee_len & 0x7FFF;
            uint64_t phys = ext4_extent_get_pblock(&exts[i]);
            if (phys > UINT32_MAX)
                return -1;
            for (uint16_t block = 0; block < len; block++) {
                if (ext2_free_block(mnt, (uint32_t)phys + block) != 0)
                    return -1;
            }
            ext4_extent_unaccount(mnt, inode, len);
        }
        hdr->eh_entries = 0;
        return 0;
    }

    ext4_extent_idx_t *idxs = (ext4_extent_idx_t *)(hdr + 1);
    uint8_t *buf = kmalloc(mnt->block_size);
    if (!buf)
        return -1;

    for (uint16_t i = 0; i < hdr->eh_entries; i++) {
        uint64_t phys = ext4_idx_get_pblock(&idxs[i]);
        if (!phys || phys > UINT32_MAX ||
            ext2_read_block(mnt, (uint32_t)phys, buf) != 0) {
            kfree(buf);
            return -1;
        }
        if (ext4_extent_free_node(mnt, (ext4_extent_header_t *)buf,
                                  inode) != 0 ||
            ext2_free_block(mnt, (uint32_t)phys) != 0) {
            kfree(buf);
            return -1;
        }
        ext4_extent_unaccount(mnt, inode, 1);
    }

    hdr->eh_entries = 0;
    kfree(buf);
    return 0;
}

int ext4_extent_free_all(ext2_mount_t *mnt, ext2_inode_t *inode) {
    if (!mnt || !inode || !ext4_inode_has_extents(inode))
        return -1;
    ext4_extent_header_t *root = (ext4_extent_header_t *)inode->i_block;
    if (ext4_extent_free_node(mnt, root, inode) != 0)
        return -1;
    ext4_extent_init_inode(inode);
    inode->i_blocks = 0;
    inode->i_size = 0;
    return 0;
}

static int ext4_extent_trim_node(ext2_mount_t *mnt,
                                 ext4_extent_header_t *hdr,
                                 uint32_t keep_blocks,
                                 ext2_inode_t *inode) {
    if (hdr->eh_magic != EXT4_EXT_MAGIC)
        return -1;

    if (hdr->eh_depth == 0) {
        ext4_extent_t *exts = (ext4_extent_t *)(hdr + 1);
        uint16_t index = 0;
        while (index < hdr->eh_entries) {
            ext4_extent_t *ex = &exts[index];
            uint16_t len = ex->ee_len & 0x7FFF;
            uint32_t end = ex->ee_block + len;
            uint16_t free_from = len;

            if (ex->ee_block >= keep_blocks)
                free_from = 0;
            else if (end > keep_blocks)
                free_from = (uint16_t)(keep_blocks - ex->ee_block);

            if (free_from == len) {
                index++;
                continue;
            }

            uint64_t phys = ext4_extent_get_pblock(ex);
            if (phys > UINT32_MAX)
                return -1;
            uint16_t free_count = len - free_from;
            for (uint16_t block = free_from; block < len; block++) {
                if (ext2_free_block(mnt, (uint32_t)phys + block) != 0)
                    return -1;
            }
            ext4_extent_unaccount(mnt, inode, free_count);

            if (free_from) {
                ex->ee_len = (ex->ee_len & 0x8000) | free_from;
                index++;
            } else {
                for (uint16_t move = index;
                     move + 1 < hdr->eh_entries; move++)
                    exts[move] = exts[move + 1];
                hdr->eh_entries--;
            }
        }
        return 0;
    }

    ext4_extent_idx_t *idxs = (ext4_extent_idx_t *)(hdr + 1);
    uint8_t *buf = kmalloc(mnt->block_size);
    if (!buf)
        return -1;

    uint16_t index = 0;
    while (index < hdr->eh_entries) {
        uint64_t phys = ext4_idx_get_pblock(&idxs[index]);
        if (!phys || phys > UINT32_MAX ||
            ext2_read_block(mnt, (uint32_t)phys, buf) != 0) {
            kfree(buf);
            return -1;
        }

        ext4_extent_header_t *child = (ext4_extent_header_t *)buf;
        if (ext4_extent_trim_node(mnt, child, keep_blocks, inode) != 0) {
            kfree(buf);
            return -1;
        }

        if (!child->eh_entries) {
            if (ext2_free_block(mnt, (uint32_t)phys) != 0) {
                kfree(buf);
                return -1;
            }
            ext4_extent_unaccount(mnt, inode, 1);
            for (uint16_t move = index;
                 move + 1 < hdr->eh_entries; move++)
                idxs[move] = idxs[move + 1];
            hdr->eh_entries--;
            continue;
        }

        idxs[index].ei_block = ext4_extent_first_logical(child);
        if (ext4_extent_write_node(mnt, (uint32_t)phys, buf) != 0) {
            kfree(buf);
            return -1;
        }
        index++;
    }

    kfree(buf);
    return 0;
}

static int ext4_extent_collapse_root(ext2_mount_t *mnt,
                                     ext2_inode_t *inode) {
    ext4_extent_header_t *root = (ext4_extent_header_t *)inode->i_block;
    if (!root->eh_depth || root->eh_entries != 1)
        return 0;

    ext4_extent_idx_t *idx = (ext4_extent_idx_t *)(root + 1);
    uint64_t phys = ext4_idx_get_pblock(idx);
    if (!phys || phys > UINT32_MAX)
        return -1;

    uint8_t *buf = kmalloc(mnt->block_size);
    if (!buf)
        return -1;
    if (ext2_read_block(mnt, (uint32_t)phys, buf) != 0) {
        kfree(buf);
        return -1;
    }

    ext4_extent_header_t *child = (ext4_extent_header_t *)buf;
    if (child->eh_magic != EXT4_EXT_MAGIC || child->eh_entries > 4) {
        kfree(buf);
        return 0;
    }

    uint16_t entries = child->eh_entries;
    uint16_t depth = child->eh_depth;
    uint32_t generation = child->eh_generation;
    memset(inode->i_block, 0, sizeof(inode->i_block));
    root = (ext4_extent_header_t *)inode->i_block;
    root->eh_magic = EXT4_EXT_MAGIC;
    root->eh_entries = entries;
    root->eh_max = 4;
    root->eh_depth = depth;
    root->eh_generation = generation;
    memcpy(root + 1, child + 1, entries * sizeof(ext4_extent_t));

    int result = ext2_free_block(mnt, (uint32_t)phys);
    if (result == 0)
        ext4_extent_unaccount(mnt, inode, 1);
    kfree(buf);
    return result;
}

int ext4_extent_truncate(ext2_mount_t *mnt, ext2_inode_t *inode,
                         uint32_t new_len) {
    if (!mnt || !inode || !ext4_inode_has_extents(inode))
        return -1;
    if (new_len >= inode->i_size) {
        inode->i_size = new_len;
        return 0;
    }

    if (!new_len)
        return ext4_extent_free_all(mnt, inode);

    uint32_t keep_blocks = (new_len + mnt->block_size - 1) / mnt->block_size;
    uint32_t tail = new_len % mnt->block_size;
    if (tail) {
        uint32_t disk_block =
            ext4_get_block_num(mnt, inode, keep_blocks - 1);
        if (disk_block) {
            uint8_t *buf = kmalloc(mnt->block_size);
            if (!buf)
                return -1;
            if (ext2_read_block(mnt, disk_block, buf) != 0) {
                kfree(buf);
                return -1;
            }
            memset(buf + tail, 0, mnt->block_size - tail);
            if (ext2_write_block(mnt, disk_block, buf) != 0) {
                kfree(buf);
                return -1;
            }
            kfree(buf);
        }
    }

    ext4_extent_header_t *root = (ext4_extent_header_t *)inode->i_block;
    if (ext4_extent_trim_node(mnt, root, keep_blocks, inode) != 0)
        return -1;
    if (!root->eh_entries)
        ext4_extent_init_inode(inode);
    else if (ext4_extent_collapse_root(mnt, inode) != 0)
        return -1;
    inode->i_size = new_len;
    return 0;
}
