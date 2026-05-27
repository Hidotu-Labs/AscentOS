#include "fs/procfs.h"
#include "apic/lapic_timer.h"
#include "drivers/storage/block.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "smp/cpu.h"

// Helper to convert an unsigned 64-bit integer to a string
static void u64_to_str(uint64_t val, char *buf) {
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char temp[32];
  int i = 0;
  while (val > 0) {
    temp[i++] = (val % 10) + '0';
    val /= 10;
  }
  int j = 0;
  while (i > 0) {
    buf[j++] = temp[--i];
  }
  buf[j] = '\0';
}

uint32_t procfs_meminfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  char buf[512];
  buf[0] = '\0';

  uint64_t total_mem_val = pmm_get_total_memory();
  uint64_t usable_mem_val = pmm_get_usable_memory();
  uint64_t free_pages_val = (uint64_t)pmm_get_free_pages();
  uint64_t free_mem_val =
      free_pages_val * 4096; // Use literal if PAGE_SIZE is causing issues

  char num_buf[32];

  // MemTotal
  strcat(buf, "MemTotal:       ");
  u64_to_str(usable_mem_val / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");

  // MemFree
  strcat(buf, "MemFree:        ");
  u64_to_str(free_mem_val / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");

  // MemAvailable
  strcat(buf, "MemAvailable:   ");
  u64_to_str(free_mem_val / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");

  strcat(buf, "Buffers:        0 kB\n");
  strcat(buf, "Cached:         0 kB\n");

  // MemUsable
  strcat(buf, "MemUsable:      ");
  u64_to_str(usable_mem_val / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");

  uint32_t len = (uint32_t)strlen(buf);
  node->length = len;

  if (offset >= len)
    return 0;
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  return size;
}

uint32_t procfs_cpuinfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  // 16KB is plenty for 64 cores
  char *buf = kmalloc(16384);
  if (!buf)
    return 0;
  buf[0] = '\0';

  uint32_t cpu_count = cpu_get_count();
  char num_buf[32];

  for (uint32_t i = 0; i < cpu_count; i++) {
    strcat(buf, "processor       : ");
    u64_to_str(i, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");

    uint32_t eax, ebx, ecx, edx;

    // Vendor ID
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));
    char vendor[13];
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';
    strcat(buf, "vendor_id       : ");
    strcat(buf, vendor);
    strcat(buf, "\n");

    // Family, Model, Stepping
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    uint32_t stepping = eax & 0xF;
    uint32_t model = (eax >> 4) & 0xF;
    uint32_t family = (eax >> 8) & 0xF;
    if (family == 0xF)
      family += (eax >> 20) & 0xFF;
    if (family == 0x6 || family == 0xF)
      model += ((eax >> 16) & 0xF) << 4;

    strcat(buf, "cpu family      : ");
    u64_to_str(family, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");

    strcat(buf, "model           : ");
    u64_to_str(model, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");

    strcat(buf, "stepping        : ");
    u64_to_str(stepping, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");

    // Brand string (Model name)
    uint32_t brand_eax;
    __asm__ volatile("cpuid" : "=a"(brand_eax) : "a"(0x80000000));
    if (brand_eax >= 0x80000004) {
      char model_name[49];
      uint32_t *mptr = (uint32_t *)model_name;
      for (uint32_t j = 0; j < 3; j++) {
        __asm__ volatile("cpuid"
                         : "=a"(mptr[j * 4]), "=b"(mptr[j * 4 + 1]),
                           "=c"(mptr[j * 4 + 2]), "=d"(mptr[j * 4 + 3])
                         : "a"(0x80000002 + j));
      }
      model_name[48] = '\0';
      // Trim leading spaces
      char *trimmed = model_name;
      while (*trimmed == ' ')
        trimmed++;
      strcat(buf, "model name      : ");
      strcat(buf, trimmed);
      strcat(buf, "\n");
    }

    strcat(buf, "flags           : fpu vme de pse tsc msr pae mce cx8 apic sep "
                "mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse "
                "sse2 ss ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc "
                "art arch_perfmon pebs bts rep_good nopl cpuid nonstop_tsc "
                "cpuid_fault tpm tm2 est immortality sse3 pclmulqdq dtes64 "
                "monitor ds_cpl vmx smx est tm2 ssse3 sdbg fma cx16 xtpr pdcm "
                "pcid sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes "
                "xsave avx f16c rdrand lahf_lm abm 3dnowprefetch\n");
    strcat(buf, "\n");
  }

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_partitions_read(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  char *buf = kmalloc(4096);
  if (!buf)
    return 0;
  buf[0] = '\0';

  strcat(buf, "major minor  #blocks  name\n\n");

  int count = block_count();
  char num_buf[32];

  for (int i = 0; i < count; i++) {
    struct block_device *dev = block_get(i);
    if (!dev)
      continue;

    strcat(buf, "   1     ");
    u64_to_str(i, num_buf);
    strcat(buf, num_buf);

    int padding = 8 - strlen(num_buf);
    for (int j = 0; j < padding; j++)
      strcat(buf, " ");

    uint64_t blocks =
        (dev->total_sectors * (dev->sector_size ? dev->sector_size : 512)) /
        1024;
    u64_to_str(blocks, num_buf);
    strcat(buf, num_buf);

    padding = 10 - strlen(num_buf);
    for (int j = 0; j < padding; j++)
      strcat(buf, " ");

    strcat(buf, dev->name);
    strcat(buf, "\n");
  }

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_mounts_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  char *buf = kmalloc(512);
  if (!buf)
    return 0;
  buf[0] = '\0';

  strcat(buf, "/dev/sata01 / ext2 rw,relatime 0 0\n");
  strcat(buf, "proc /proc procfs rw,relatime 0 0\n");

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_uptime_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  char buf[64];
  uint64_t ms = lapic_timer_get_ms();

  // Format: "up.time idle.time"
  u64_to_str(ms / 1000, buf);
  strcat(buf, ".");
  uint32_t rem = (ms % 1000) / 10;
  if (rem < 10)
    strcat(buf, "0");
  u64_to_str(rem, buf + strlen(buf));
  strcat(buf, " 0.00\n");

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len)
    return 0;
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  return size;
}

uint32_t procfs_stat_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
  char *buf = kmalloc(1024);
  if (!buf)
    return 0;
  buf[0] = '\0';

  strcat(buf, "cpu  100 0 100 1000 0 0 0 0 0 0\n");

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_heapinfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
  // 4KB should be plenty for heap info
  char *buf = kmalloc(4096);
  if (!buf)
    return 0;

  heap_get_info(buf);

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_cmdline_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  (void)node;
  const char *cmd = "Xfbdev\n";
  uint32_t len = (uint32_t)strlen(cmd);

  if (offset >= len)
    return 0;
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, cmd + offset, size);
  return size;
}

void procfs_init(void) {
  if (!fs_root)
    return;

  // Try to find /proc, create if it doesn't exist (assuming ext2/3 supports
  // root->mkdir)
  vfs_node_t *proc_dir = vfs_finddir(fs_root, "proc");
  if (!proc_dir && fs_root->mkdir) {
    fs_root->mkdir(fs_root, "proc", 0755);
    proc_dir = vfs_finddir(fs_root, "proc");
  }

  if (proc_dir) {
    // Stage a new virtual root for procfs
    vfs_node_t *procfs_root = kmalloc(sizeof(vfs_node_t));
    vfs_node_init(procfs_root);
    strcpy(procfs_root->name, "proc");
    ramfs_mount_on(procfs_root);

    // Apply the mount: anyone looking up 'proc' will now get our virtual root
    vfs_mount(proc_dir, procfs_root);

    // Add /proc/meminfo
    vfs_node_t *meminfo_node = kmalloc(sizeof(vfs_node_t));
    if (meminfo_node) {
      vfs_node_init(meminfo_node);
      strncpy(meminfo_node->name, "meminfo", 127);
      meminfo_node->flags = FS_FILE | FS_PERSISTENT;
      meminfo_node->mask = 0444; // Read-only
      meminfo_node->read = procfs_meminfo_read;
      meminfo_node->length = 512; // Dummy size, redefined on read

      ramfs_mount_node(procfs_root, meminfo_node);
    }

    // Add /proc/cpuinfo
    vfs_node_t *cpuinfo_node = kmalloc(sizeof(vfs_node_t));
    if (cpuinfo_node) {
      vfs_node_init(cpuinfo_node);
      strncpy(cpuinfo_node->name, "cpuinfo", 127);
      cpuinfo_node->flags = FS_FILE | FS_PERSISTENT;
      cpuinfo_node->mask = 0444; // Read-only
      cpuinfo_node->read = procfs_cpuinfo_read;
      cpuinfo_node->length = 2048; // Dummy size

      ramfs_mount_node(procfs_root, cpuinfo_node);
    }

    // Add /proc/partitions
    vfs_node_t *part_node = kmalloc(sizeof(vfs_node_t));
    if (part_node) {
      vfs_node_init(part_node);
      strncpy(part_node->name, "partitions", 127);
      part_node->flags = FS_FILE | FS_PERSISTENT;
      part_node->mask = 0444; // Read-only
      part_node->read = procfs_partitions_read;
      part_node->length = 1024; // Dummy size

      ramfs_mount_node(procfs_root, part_node);
    }

    // Add /proc/mounts
    vfs_node_t *mounts_node = kmalloc(sizeof(vfs_node_t));
    if (mounts_node) {
      vfs_node_init(mounts_node);
      strncpy(mounts_node->name, "mounts", 127);
      mounts_node->flags = FS_FILE | FS_PERSISTENT;
      mounts_node->mask = 0444;
      mounts_node->read = procfs_mounts_read;
      ramfs_mount_node(procfs_root, mounts_node);
    }

    // Add /proc/uptime
    vfs_node_t *uptime_node = kmalloc(sizeof(vfs_node_t));
    if (uptime_node) {
      vfs_node_init(uptime_node);
      strncpy(uptime_node->name, "uptime", 127);
      uptime_node->flags = FS_FILE | FS_PERSISTENT;
      uptime_node->mask = 0444;
      uptime_node->read = procfs_uptime_read;
      ramfs_mount_node(procfs_root, uptime_node);
    }

    // Add /proc/stat
    vfs_node_t *static_node = kmalloc(sizeof(vfs_node_t));
    if (static_node) {
      vfs_node_init(static_node);
      strncpy(static_node->name, "stat", 127);
      static_node->flags = FS_FILE | FS_PERSISTENT;
      static_node->mask = 0444;
      static_node->read = procfs_stat_read;
      ramfs_mount_node(procfs_root, static_node);
    }

    // Add /proc/heapinfo
    vfs_node_t *heapinfo_node = kmalloc(sizeof(vfs_node_t));
    if (heapinfo_node) {
      vfs_node_init(heapinfo_node);
      strncpy(heapinfo_node->name, "heapinfo", 127);
      heapinfo_node->flags = FS_FILE | FS_PERSISTENT;
      heapinfo_node->mask = 0444;
      heapinfo_node->read = procfs_heapinfo_read;
      ramfs_mount_node(procfs_root, heapinfo_node);
    }

    // Add /proc/cmdline
    vfs_node_t *cmdline_node = kmalloc(sizeof(vfs_node_t));
    if (cmdline_node) {
      vfs_node_init(cmdline_node);
      strncpy(cmdline_node->name, "cmdline", 127);
      cmdline_node->flags = FS_FILE | FS_PERSISTENT;
      cmdline_node->mask = 0444;
      cmdline_node->read = procfs_cmdline_read;
      ramfs_mount_node(procfs_root, cmdline_node);
    }

    // Add /proc/self directory
    vfs_node_t *self_dir = kmalloc(sizeof(vfs_node_t));
    if (self_dir) {
      vfs_node_init(self_dir);
      strncpy(self_dir->name, "self", 127);
      self_dir->flags = FS_DIRECTORY | FS_PERSISTENT;
      self_dir->mask = 0555;
      ramfs_mount_on(self_dir); // Crucial: Initialize directory structure!
      ramfs_mount_node(procfs_root, self_dir);

      // Add /proc/self/cmdline
      vfs_node_t *self_cmdline = kmalloc(sizeof(vfs_node_t));
      if (self_cmdline) {
        vfs_node_init(self_cmdline);
        strncpy(self_cmdline->name, "cmdline", 127);
        self_cmdline->flags = FS_FILE | FS_PERSISTENT;
        self_cmdline->mask = 0444;
        self_cmdline->read = procfs_cmdline_read;
        ramfs_mount_node(self_dir, self_cmdline);
      }
    }
  }
}
