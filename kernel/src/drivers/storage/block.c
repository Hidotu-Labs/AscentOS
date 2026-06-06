#include "drivers/storage/block.h"
#include <stddef.h>

static struct block_device *registered[BLOCK_MAX_DEVICES];
static int num_devices = 0;

#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"

static uint32_t block_vfs_read(struct vfs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  struct block_device *dev = (struct block_device *)node->device;
  if (!dev || !dev->read_sectors)
    return 0;

  uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
  uint32_t sector = offset / sector_size;
  uint32_t count = size / sector_size;

  if (size % sector_size != 0)
    return 0; // Enforce sector aligned logical reads for now

  int err = dev->read_sectors(dev, sector, count, buffer);
  if (err)
    return 0;
  return size;
}

// Partition Wrapper

struct partition_wrapper {
  struct block_device *parent;
  uint64_t start_lba;
};

static int partition_read(struct block_device *dev, uint64_t lba,
                          uint32_t count, void *buf) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  return wrap->parent->read_sectors(wrap->parent, lba + wrap->start_lba, count,
                                    buf);
}

static int partition_write(struct block_device *dev, uint64_t lba,
                           uint32_t count, const void *buf) {
  struct partition_wrapper *wrap = (struct partition_wrapper *)dev->driver_data;
  if (!wrap->parent->write_sectors)
    return -1;
  return wrap->parent->write_sectors(wrap->parent, lba + wrap->start_lba, count,
                                     buf);
}

// MBR Structures

struct mbr_partition {
  uint8_t status;
  uint8_t start_chs[3];
  uint8_t type;
  uint8_t end_chs[3];
  uint32_t start_lba;
  uint32_t total_sectors;
} __attribute__((packed));

struct mbr {
  uint8_t bootstrap[446];
  struct mbr_partition partitions[4];
  uint16_t signature;
} __attribute__((packed));

void block_scan_partitions(struct block_device *dev) {
  if (!dev->read_sectors)
    return;

  struct mbr *mbr = kmalloc(sizeof(struct mbr));
  if (!mbr)
    return;

  if (dev->read_sectors(dev, 0, 1, mbr) != 0) {
    kfree(mbr);
    return;
  }

  if (mbr->signature != 0xAA55) {
    kfree(mbr);
    return;
  }

  for (int i = 0; i < 4; i++) {
    struct mbr_partition *p = &mbr->partitions[i];
    if (p->type == 0 || p->total_sectors == 0)
      continue;

    struct partition_wrapper *wrap = kmalloc(sizeof(struct partition_wrapper));
    if (!wrap)
      continue;

    wrap->parent = dev;
    wrap->start_lba = p->start_lba;

    struct block_device *pdev = kmalloc(sizeof(struct block_device));
    if (!pdev) {
      kfree(wrap);
      continue;
    }

    memset(pdev, 0, sizeof(struct block_device));

    // Name: "sda" + "1" -> "sda1"
    strncpy(pdev->name, dev->name, 12);
    int len = strlen(pdev->name);
    pdev->name[len] = '1' + i;
    pdev->name[len + 1] = '\0';

    pdev->sector_size = dev->sector_size;
    pdev->total_sectors = p->total_sectors;
    pdev->read_sectors = partition_read;
    if (dev->write_sectors)
      pdev->write_sectors = partition_write;
    pdev->driver_data = wrap;

    block_register(pdev);
  }

  kfree(mbr);
}

int block_register(struct block_device *dev) {
  if (num_devices >= BLOCK_MAX_DEVICES)
    return -1;
  registered[num_devices++] = dev;

  // Register to VFS if root exists
  if (fs_root) {
    vfs_node_t *dev_dir = vfs_finddir(fs_root, "dev");
    if (dev_dir) {
      vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
      vfs_node_init(node);
      strncpy(node->name, dev->name, 127);
      node->flags = FS_BLOCKDEV;
      node->mask = 0600;
      node->length =
          dev->total_sectors * (dev->sector_size ? dev->sector_size : 512);
      node->device = dev;
      node->read = block_vfs_read;
      ramfs_mount_node(dev_dir, node);
    }
  }

  // Auto-scan for partitions if this isn't already a partition
  if (dev->read_sectors != partition_read) {
    block_scan_partitions(dev);
  }

  return 0;
}

struct block_device *block_get(int index) {
  if (index < 0 || index >= num_devices)
    return NULL;
  return registered[index];
}

int block_count(void) { return num_devices; }

void block_repopulate_devices(void) {
  if (!fs_root)
    return;

  vfs_node_t *dev_dir = vfs_finddir(fs_root, "dev");
  if (!dev_dir)
    return;

  // Re-register all devices to the new /dev
  for (int i = 0; i < num_devices; i++) {
    struct block_device *dev = registered[i];
    if (!dev)
      continue;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node)
      continue;
    vfs_node_init(node);
    strncpy(node->name, dev->name, 127);
    node->flags = FS_BLOCKDEV;
    node->mask = 0600;
    node->length =
        dev->total_sectors * (dev->sector_size ? dev->sector_size : 512);
    node->device = dev;
    node->read = block_vfs_read;

    // Use vfs_create if available (ext2), otherwise ramfs_mount_node
    if (dev_dir->create) {
      dev_dir->create(dev_dir, dev->name, 0600);
      vfs_node_t *new_node = vfs_finddir(dev_dir, dev->name);
      if (new_node) {
        new_node->flags = FS_BLOCKDEV;
        new_node->device = dev;
        new_node->read = block_vfs_read;
        new_node->length = node->length;
      }
      kfree(node);
    } else {
      ramfs_mount_node(dev_dir, node);
    }
  }
}
