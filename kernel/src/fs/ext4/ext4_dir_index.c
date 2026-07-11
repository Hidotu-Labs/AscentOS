#include "ext4_dir_index.h"
#include "ext4_extent.h"
#include "fs/ext2/ext2_internal.h"
#include "lib/string.h"
#include "mm/heap.h"

#include <stdint.h>

#define MD4_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define MD4_G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define MD4_H(x, y, z) ((x) ^ (y) ^ (z))

#define ROL32(value, shift) \
    (((value) << (shift)) | ((value) >> (32 - (shift))))

#define MD4_ROUND(function, a, b, c, d, value, shift) \
    do {                                                   \
        (a) += function((b), (c), (d)) + (value);          \
        (a) = ROL32((a), (shift));                         \
    } while (0)

#define MD4_K2 013240474631u
#define MD4_K3 015666365641u

typedef struct {
    uint32_t zero;
    uint8_t version;
    uint8_t length;
    uint8_t levels;
    uint8_t flags;
} __attribute__((packed)) dx_info_t;

typedef struct {
    uint16_t limit;
    uint16_t count;
} __attribute__((packed)) dx_count_limit_t;

typedef struct {
    uint32_t hash;
    uint32_t block;
} __attribute__((packed)) dx_entry_t;

typedef struct {
    uint32_t hash;
    uint32_t inode;
    uint16_t size;
    uint8_t name_len;
    uint8_t type;
    char name[256];
} dx_item_t;

static uint32_t dirent_min_size(uint32_t name_length)
{
    return (8u + name_length + 3u) & ~3u;
}

static void build_hash_buffer(
    const char *name,
    int name_length,
    uint32_t *output,
    int output_words,
    int unsigned_chars)
{
    uint32_t padding;
    uint32_t value;
    int words_left;

    padding = (uint32_t)name_length |
              ((uint32_t)name_length << 8);

    padding |= padding << 16;

    value = padding;
    words_left = output_words;

    if (name_length > output_words * 4)
        name_length = output_words * 4;

    for (int i = 0; i < name_length; i++) {
        int character;

        if (unsigned_chars)
            character = (int)(uint8_t)name[i];
        else
            character = (int)(int8_t)name[i];

        value = (uint32_t)character + (value << 8);

        if ((i & 3) == 3) {
            *output++ = value;
            value = padding;
            words_left--;
        }
    }

    if (--words_left >= 0)
        *output++ = value;

    while (--words_left >= 0)
        *output++ = padding;
}

static void half_md4_transform(
    uint32_t state[4],
    const uint32_t input[8])
{
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    /* Round 1 */

    MD4_ROUND(MD4_F, a, b, c, d, input[0], 3);
    MD4_ROUND(MD4_F, d, a, b, c, input[1], 7);
    MD4_ROUND(MD4_F, c, d, a, b, input[2], 11);
    MD4_ROUND(MD4_F, b, c, d, a, input[3], 19);

    MD4_ROUND(MD4_F, a, b, c, d, input[4], 3);
    MD4_ROUND(MD4_F, d, a, b, c, input[5], 7);
    MD4_ROUND(MD4_F, c, d, a, b, input[6], 11);
    MD4_ROUND(MD4_F, b, c, d, a, input[7], 19);

    /* Round 2 */

    MD4_ROUND(MD4_G, a, b, c, d, input[1] + MD4_K2, 3);
    MD4_ROUND(MD4_G, d, a, b, c, input[3] + MD4_K2, 5);
    MD4_ROUND(MD4_G, c, d, a, b, input[5] + MD4_K2, 9);
    MD4_ROUND(MD4_G, b, c, d, a, input[7] + MD4_K2, 13);

    MD4_ROUND(MD4_G, a, b, c, d, input[0] + MD4_K2, 3);
    MD4_ROUND(MD4_G, d, a, b, c, input[2] + MD4_K2, 5);
    MD4_ROUND(MD4_G, c, d, a, b, input[4] + MD4_K2, 9);
    MD4_ROUND(MD4_G, b, c, d, a, input[6] + MD4_K2, 13);

    /* Round 3 */

    MD4_ROUND(MD4_H, a, b, c, d, input[3] + MD4_K3, 3);
    MD4_ROUND(MD4_H, d, a, b, c, input[7] + MD4_K3, 9);
    MD4_ROUND(MD4_H, c, d, a, b, input[2] + MD4_K3, 11);
    MD4_ROUND(MD4_H, b, c, d, a, input[6] + MD4_K3, 15);

    MD4_ROUND(MD4_H, a, b, c, d, input[1] + MD4_K3, 3);
    MD4_ROUND(MD4_H, d, a, b, c, input[5] + MD4_K3, 9);
    MD4_ROUND(MD4_H, c, d, a, b, input[0] + MD4_K3, 11);
    MD4_ROUND(MD4_H, b, c, d, a, input[4] + MD4_K3, 15);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static int ext4_directory_hash(
    ext2_mount_t *mount,
    uint8_t version,
    const char *name,
    int name_length,
    uint32_t *result_hash)
{
    uint32_t state[4] = {
        0x67452301,
        0xefcdab89,
        0x98badcfe,
        0x10325476
    };

    const char *current;
    int bytes_left;

    if (version != 1 && version != 4)
        return -1;

    if (mount->sb.s_hash_seed[0] ||
        mount->sb.s_hash_seed[1] ||
        mount->sb.s_hash_seed[2] ||
        mount->sb.s_hash_seed[3]) {
        memcpy(
            state,
            mount->sb.s_hash_seed,
            sizeof(state)
        );
    }

    current = name;
    bytes_left = name_length;

    while (bytes_left > 0) {
        uint32_t hash_input[8];

        build_hash_buffer(
            current,
            bytes_left,
            hash_input,
            8,
            version == 4
        );

        half_md4_transform(state, hash_input);

        current += 32;
        bytes_left -= 32;
    }

    *result_hash = state[1] & 0xfffffffeu;

    if (*result_hash == 0xfffffffeu)
        *result_hash = 0xfffffffcu;

    return 0;
}

static int insert_into_leaf(
    uint8_t *block_buffer,
    uint32_t block_size,
    uint32_t inode_number,
    const char *name,
    uint8_t name_length,
    uint8_t file_type)
{
    uint32_t required_size = dirent_min_size(name_length);

    for (uint32_t offset = 0; offset < block_size;) {
        ext2_dirent_t *entry;
        uint32_t used_size;

        entry = (ext2_dirent_t *)(block_buffer + offset);

        if (entry->rec_len < 8 ||
            offset + entry->rec_len > block_size) {
            return -1;
        }

        /*
         * Reuse an entirely unused directory entry.
         */
        if (entry->inode == 0 && entry->rec_len >= required_size) {
            uint16_t record_length = entry->rec_len;

            memset(entry, 0, record_length);

            entry->inode = inode_number;
            entry->rec_len = record_length;
            entry->name_len = name_length;
            entry->file_type = file_type;

            memcpy(entry->name, name, name_length);

            return 0;
        }

        /*
         * Split the unused tail space from an existing entry.
         */
        used_size = dirent_min_size(entry->name_len);

        if (entry->rec_len >= used_size + required_size) {
            uint16_t old_record_length = entry->rec_len;
            ext2_dirent_t *new_entry;

            entry->rec_len = used_size;

            new_entry = (ext2_dirent_t *)(
                block_buffer + offset + used_size
            );

            memset(
                new_entry,
                0,
                old_record_length - used_size
            );

            new_entry->inode = inode_number;
            new_entry->rec_len = old_record_length - used_size;
            new_entry->name_len = name_length;
            new_entry->file_type = file_type;

            memcpy(new_entry->name, name, name_length);

            return 0;
        }

        offset += entry->rec_len;
    }

    /*
     * The leaf is valid, but has no free space.
     */
    return 1;
}

static int collect_leaf_items(
    ext2_mount_t *mount,
    uint8_t hash_version,
    uint8_t *block_buffer,
    uint32_t block_size,
    dx_item_t *items,
    uint32_t item_capacity,
    uint32_t *item_count)
{
    for (uint32_t offset = 0; offset < block_size;) {
        ext2_dirent_t *entry;

        entry = (ext2_dirent_t *)(block_buffer + offset);

        if (entry->rec_len < 8 ||
            offset + entry->rec_len > block_size) {
            return -1;
        }

        if (entry->inode != 0) {
            dx_item_t *item;

            if (*item_count >= item_capacity)
                return -1;

            item = &items[(*item_count)++];

            if (ext4_directory_hash(
                    mount,
                    hash_version,
                    entry->name,
                    entry->name_len,
                    &item->hash) != 0) {
                return -1;
            }

            item->inode = entry->inode;
            item->name_len = entry->name_len;
            item->type = entry->file_type;
            item->size = dirent_min_size(entry->name_len);

            memcpy(
                item->name,
                entry->name,
                entry->name_len
            );
        }

        offset += entry->rec_len;
    }

    return 0;
}

static void sort_items_by_hash(
    dx_item_t *items,
    uint32_t item_count)
{
    /*
     * Insertion sort is sufficient here because a single directory
     * leaf generally contains a relatively small number of entries.
     */
    for (uint32_t i = 1; i < item_count; i++) {
        dx_item_t current = items[i];
        uint32_t position = i;

        while (position > 0 &&
               items[position - 1].hash > current.hash) {
            items[position] = items[position - 1];
            position--;
        }

        items[position] = current;
    }
}

static int pack_leaf_items(
    uint8_t *block_buffer,
    uint32_t block_size,
    dx_item_t *items,
    uint32_t first_item,
    uint32_t end_item)
{
    uint32_t offset = 0;

    if (first_item == end_item)
        return -1;

    memset(block_buffer, 0, block_size);

    for (uint32_t i = first_item; i < end_item; i++) {
        ext2_dirent_t *entry;
        uint32_t record_length;

        if (i + 1 == end_item)
            record_length = block_size - offset;
        else
            record_length = items[i].size;

        if (record_length < items[i].size ||
            offset + record_length > block_size) {
            return -1;
        }

        entry = (ext2_dirent_t *)(block_buffer + offset);

        entry->inode = items[i].inode;
        entry->rec_len = record_length;
        entry->name_len = items[i].name_len;
        entry->file_type = items[i].type;

        memcpy(
            entry->name,
            items[i].name,
            entry->name_len
        );

        offset += record_length;
    }

    return 0;
}

int ext4_dx_add_entry(
    ext2_mount_t *mount,
    uint32_t directory_inode_number,
    ext2_inode_t *directory_inode,
    uint32_t child_inode_number,
    const char *name,
    uint8_t file_type)
{
    uint32_t name_length;
    uint32_t item_capacity;

    uint8_t *root_buffer = NULL;
    uint8_t *leaf_buffer = NULL;
    uint8_t *right_buffer = NULL;

    dx_item_t *items = NULL;

    int result = -1;

    name_length = strlen(name);

    if (name_length == 0 ||
        name_length > 255 ||
        !(directory_inode->i_flags & EXT2_INDEX_FL)) {
        return -1;
    }

    root_buffer = kmalloc(mount->block_size);
    leaf_buffer = kmalloc(mount->block_size);
    right_buffer = kmalloc(mount->block_size);

    item_capacity = mount->block_size / 8 + 1;
    items = kmalloc(item_capacity * sizeof(*items));

    if (!root_buffer ||
        !leaf_buffer ||
        !right_buffer ||
        !items) {
        goto cleanup;
    }

    /*
     * Read and validate the HTree root block.
     */
    uint32_t root_physical_block = ext2_get_block_num(
        mount,
        directory_inode,
        0
    );

    if (root_physical_block == 0 ||
        ext2_read_block(
            mount,
            root_physical_block,
            root_buffer) != 0) {
        goto cleanup;
    }

    dx_info_t *root_info =
        (dx_info_t *)(root_buffer + 24);

    dx_count_limit_t *count_limit =
        (dx_count_limit_t *)(root_buffer + 32);

    dx_entry_t *entries =
        (dx_entry_t *)(root_buffer + 32);

    if (root_info->zero != 0 ||
        root_info->length != 8 ||
        root_info->levels != 0 ||
        count_limit->count == 0 ||
        count_limit->count > count_limit->limit ||
        32u + (uint32_t)count_limit->limit * sizeof(dx_entry_t) >
            mount->block_size) {
        goto cleanup;
    }

    /*
     * Calculate the hash of the new entry and locate its target leaf.
     */
    uint32_t name_hash;

    if (ext4_directory_hash(
            mount,
            root_info->version,
            name,
            name_length,
            &name_hash) != 0) {
        goto cleanup;
    }

    uint16_t target_index = 0;

    for (uint16_t i = 1; i < count_limit->count; i++) {
        if (name_hash < entries[i].hash)
            break;

        target_index = i;
    }

    uint32_t leaf_logical_block =
        entries[target_index].block & 0x0fffffffu;

    uint32_t leaf_physical_block = ext2_get_block_num(
        mount,
        directory_inode,
        leaf_logical_block
    );

    if (leaf_physical_block == 0 ||
        ext2_read_block(
            mount,
            leaf_physical_block,
            leaf_buffer) != 0) {
        goto cleanup;
    }

    /*
     * First try inserting without splitting the leaf.
     */
    int insert_result = insert_into_leaf(
        leaf_buffer,
        mount->block_size,
        child_inode_number,
        name,
        name_length,
        file_type
    );

    if (insert_result == 0) {
        result = ext3_journal_block(
            mount,
            leaf_physical_block,
            leaf_buffer
        );

        goto cleanup;
    }

    /*
     * A negative result means that the leaf block was malformed.
     * A full root index cannot accept another leaf pointer.
     */
    if (insert_result < 0 ||
        count_limit->count >= count_limit->limit) {
        goto cleanup;
    }

    /*
     * Collect all existing entries and append the new entry.
     */
    uint32_t item_count = 0;

    if (collect_leaf_items(
            mount,
            root_info->version,
            leaf_buffer,
            mount->block_size,
            items,
            item_capacity,
            &item_count) != 0 ||
        item_count >= item_capacity) {
        goto cleanup;
    }

    dx_item_t *new_item = &items[item_count++];

    new_item->hash = name_hash;
    new_item->inode = child_inode_number;
    new_item->name_len = name_length;
    new_item->type = file_type;
    new_item->size = dirent_min_size(name_length);

    memcpy(new_item->name, name, name_length);

    sort_items_by_hash(items, item_count);

    /*
     * Find a split point that keeps the used byte count approximately
     * balanced between the two leaves.
     */
    uint32_t total_bytes = 0;

    for (uint32_t i = 0; i < item_count; i++)
        total_bytes += items[i].size;

    uint32_t split_index = 1;
    uint32_t left_bytes = items[0].size;

    while (split_index + 1 < item_count &&
           left_bytes + items[split_index].size <
               total_bytes / 2) {
        left_bytes += items[split_index].size;
        split_index++;
    }

    if (pack_leaf_items(
            leaf_buffer,
            mount->block_size,
            items,
            0,
            split_index) != 0 ||
        pack_leaf_items(
            right_buffer,
            mount->block_size,
            items,
            split_index,
            item_count) != 0) {
        goto cleanup;
    }

    /*
     * Allocate a new logical block for the right-hand leaf.
     */
    uint32_t new_logical_block =
        directory_inode->i_size / mount->block_size;

    uint64_t new_physical_block;

    if (ext4_alloc_extent(
            mount,
            directory_inode,
            directory_inode_number,
            new_logical_block,
            1,
            &new_physical_block) != 0 ||
        new_physical_block > UINT32_MAX) {
        goto cleanup;
    }

    /*
     * Add the new split boundary to the root index.
     *
     * The low bit marks a continuation when the same hash appears on
     * both sides of the split.
     */
    uint32_t boundary_hash = items[split_index].hash;

    if (items[split_index - 1].hash == boundary_hash)
        boundary_hash |= 1;

    for (uint16_t i = count_limit->count;
         i > target_index + 1;
         i--) {
        entries[i] = entries[i - 1];
    }

    entries[target_index + 1].hash = boundary_hash;
    entries[target_index + 1].block = new_logical_block;

    count_limit->count++;

    directory_inode->i_size += mount->block_size;
    directory_inode->i_blocks += mount->block_size / 512;

    /*
     * Journal both leaves, the updated root, and finally the inode.
     */
    result = ext3_journal_block(
        mount,
        leaf_physical_block,
        leaf_buffer
    );

    if (result == 0) {
        result = ext3_journal_block(
            mount,
            (uint32_t)new_physical_block,
            right_buffer
        );
    }

    if (result == 0) {
        result = ext3_journal_block(
            mount,
            root_physical_block,
            root_buffer
        );
    }

    if (result == 0) {
        result = ext2_write_inode(
            mount,
            directory_inode_number,
            directory_inode
        );
    }

cleanup:
    if (root_buffer)
        kfree(root_buffer);

    if (leaf_buffer)
        kfree(leaf_buffer);

    if (right_buffer)
        kfree(right_buffer);

    if (items)
        kfree(items);

    return result;
}