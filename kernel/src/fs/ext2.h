#ifndef FS_EXT2_H
#define FS_EXT2_H

#include "drivers/storage/block.h"
#include "vfs.h"
#include <stdbool.h>
#include <stdint.h>

// Ext2 Constants

#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INODE 2
#define EXT2_DIRECT_BLOCKS 12
#define EXT2_GOOD_OLD_INODE_SIZE 128

// Inode type bits (upper 4 bits of i_mode)
#define EXT2_S_IFREG 0x8000  // Regular file
#define EXT2_S_IFDIR 0x4000  // Directory
#define EXT2_S_IFLNK 0xA000  // Symbolic link
#define EXT2_S_IFSOCK 0xC000 // Socket
#define EXT2_S_IFIFO 0x1000  // FIFO

// Directory entry file type (d_file_type)
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR 2
#define EXT2_FT_CHRDEV 3
#define EXT2_FT_BLKDEV 4
#define EXT2_FT_FIFO 5
#define EXT2_FT_SOCK 6
#define EXT2_FT_SYMLINK 7

// On-Disk Structures

typedef struct {
  uint32_t s_inodes_count;      // Total number of inodes
  uint32_t s_blocks_count;      // Total number of blocks
  uint32_t s_r_blocks_count;    // Reserved blocks
  uint32_t s_free_blocks_count; // Free blocks
  uint32_t s_free_inodes_count; // Free inodes
  uint32_t s_first_data_block;  // First data block (0 for 4K, 1 for 1K)
  uint32_t s_log_block_size;    // Block size = 1024 << this
  uint32_t s_log_frag_size;     // Fragment size
  uint32_t s_blocks_per_group;  // Blocks per group
  uint32_t s_frags_per_group;   // Fragments per group
  uint32_t s_inodes_per_group;  // Inodes per group
  uint32_t s_mtime;             // Last mount time
  uint32_t s_wtime;             // Last write time
  uint16_t s_mnt_count;         // Mount count
  uint16_t s_max_mnt_count;     // Max mount count
  uint16_t s_magic;             // Magic signature (0xEF53)
  uint16_t s_state;             // FS state
  uint16_t s_errors;            // Error behaviour
  uint16_t s_minor_rev_level;   // Minor revision level
  uint32_t s_lastcheck;         // Time of last check
  uint32_t s_checkinterval;     // Max time between checks
  uint32_t s_creator_os;        // OS
  uint32_t s_rev_level;         // Revision level
  uint16_t s_def_resuid;        // Default uid for reserved blocks
  uint16_t s_def_resgid;        // Default gid for reserved blocks
  // -- EXT2_DYNAMIC_REV specific --
  uint32_t s_first_ino;         // First non-reserved inode
  uint16_t s_inode_size;        // Size of inode structure
  uint16_t s_block_group_nr;    // Block group # of this superblock
  uint32_t s_feature_compat;    // Compatible feature set
  uint32_t s_feature_incompat;  // Incompatible feature set
  uint32_t s_feature_ro_compat; // Read-only compatible feature set
  uint8_t s_uuid[16];           // 128-bit uuid for volume
  char s_volume_name[16];       // Volume name
  uint8_t s_last_mounted[64];   // Directory where last mounted
  uint32_t s_algo_bitmap;       // For compression
  uint8_t s_prealloc_blocks;
  uint8_t s_prealloc_dir_blocks;
  uint16_t s_reserved_gdt_blocks;
  uint8_t s_journal_uuid[16]; // uuid of journal superblock
  uint32_t s_journal_inum;    // inode number of journal file
  uint32_t s_journal_dev;     // device number of journal file
  uint32_t s_last_orphan;     // start of list of inodes to delete
  uint32_t s_hash_seed[4];    // HTREE hash seed
  uint8_t s_def_hash_version; // Default hash version to use
  uint8_t s_jnl_backup_type;
  uint16_t s_desc_size; // size of group descriptor
  uint32_t s_default_mount_opts;
  uint32_t s_first_meta_bg;  // First metablock block group
  uint32_t s_mkfs_time;      // When the filesystem was created
  uint32_t s_jnl_blocks[17]; // Backup of the journal inode
  // Padding to 1024 bytes (1024 - 360 = 664)
  uint8_t s_padding[664];
} __attribute__((packed)) ext2_superblock_t;

// Block Group Descriptor (32 bytes)
typedef struct {
  uint32_t bg_block_bitmap;      // Block bitmap block
  uint32_t bg_inode_bitmap;      // Inode bitmap block
  uint32_t bg_inode_table;       // Inode table start block
  uint16_t bg_free_blocks_count; // Free blocks count
  uint16_t bg_free_inodes_count; // Free inodes count
  uint16_t bg_used_dirs_count;   // Directories count
  uint16_t bg_pad;
  uint8_t bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

// Inode (128 bytes for good-old revision)
typedef struct {
  uint16_t i_mode;        // File type and access rights
  uint16_t i_uid;         // Owner user ID
  uint32_t i_size;        // Size in bytes
  uint32_t i_atime;       // Access time
  uint32_t i_ctime;       // Creation time
  uint32_t i_mtime;       // Modification time
  uint32_t i_dtime;       // Deletion time
  uint16_t i_gid;         // Owner group ID
  uint16_t i_links_count; // Hard links count
  uint32_t i_blocks;      // Number of 512-byte blocks
  uint32_t i_flags;       // File flags
  uint32_t i_osd1;        // OS-dependent value 1
  uint32_t i_block[15];  // Pointers to blocks (12 direct + indirect + dindirect
                         // + tindirect)
  uint32_t i_generation; // File version (for NFS)
  uint32_t i_file_acl;   // File ACL
  uint32_t
      i_dir_acl; // Directory ACL (or upper 32 bits of size for regular files)
  uint32_t i_faddr;   // Fragment address
  uint8_t i_osd2[12]; // OS-dependent value 2
} __attribute__((packed)) ext2_inode_t;

// Directory Entry
typedef struct {
  uint32_t inode;    // Inode number
  uint16_t rec_len;  // Total entry length
  uint8_t name_len;  // Name length
  uint8_t file_type; // File type
  char name[];       // Filename (NOT null-terminated on disk)
} __attribute__((packed)) ext2_dirent_t;

// Mount Context

typedef struct {
  struct block_device *dev;  // Underlying block device
  ext2_superblock_t sb;      // Cached superblock
  ext2_bgd_t *bgdt;          // Cached block group descriptor table
  uint32_t block_size;       // Computed block size in bytes
  uint32_t groups_count;     // Number of block groups
  uint32_t inodes_per_group; // Inodes per group
  uint32_t inode_size;       // Size of an inode on disk
  vfs_node_t *root_node;     // VFS node for the ext2 root directory
  
  // Simple block cache
  struct {
    uint32_t num;
    uint8_t *data;
  } cache[32];
} ext2_mount_t;

// Public API

// Mount an ext2 filesystem from the given block device onto the given VFS node.
// Returns 0 on success, -1 on failure.
int ext2_mount(struct block_device *dev, vfs_node_t *mountpoint);

// Mount ext2 as the root filesystem, replacing the current fs_root.
// Returns 0 on success, -1 on failure.
int ext2_mount_root(struct block_device *dev);

// Ext2 block/inode helpers exposed for ext3 journal reading
int ext2_read_block(ext2_mount_t *mnt, uint32_t block_num, void *buffer);
int ext2_write_block(ext2_mount_t *mnt, uint32_t block_num, const void *buffer);
int ext2_read_inode(ext2_mount_t *mnt, uint32_t inode_num, ext2_inode_t *out);
uint32_t ext2_get_block_num(ext2_mount_t *mnt, ext2_inode_t *inode,
                            uint32_t logical_block);

#endif
