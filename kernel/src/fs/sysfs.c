// ── sysfs.c ──────────────────────────────────────────────────────────────────
// A real (ramfs-backed) /sys implementation.
//
// Layout:
//   /sys/
//     bus/
//       pci/
//         devices/
//           0000:<bus>:<slot>.<func>/   (one per PCI device)
//             vendor   class   device   irq   resource
//     class/
//       block/
//         <name>/   (one per block device)
//           dev   size   removable
//       net/
//         <name>/   (one per NIC)
//           address   operstate   type
//       drm/
//         card0/
//           dev   uevent
//       input/
//         event0/
//           dev
//     devices/
//       system/
//         cpu/
//           cpu<N>/   (one per logical CPU)
//             online
//     dev/
//       block/   char/
//     kernel/
//       hostname
//     power/
//       state
//
// All files are backed by ramfs and written once at boot.  The mount is done
// the same way as procfs: allocate a fresh ramfs root, then vfs_mount() it
// onto the /sys directory node so the VFS redirects all lookups through it.
// ─────────────────────────────────────────────────────────────────────────────

#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/sysfs.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "drivers/storage/block.h"
#include "drivers/net/nic.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "smp/cpu.h"

// ── Helpers ───────────────────────────────────────────────────────────────

static void u32_to_hex(uint32_t val, char *buf, int width) {
  const char *hex = "0123456789abcdef";
  buf[width] = '\0';
  for (int i = width - 1; i >= 0; i--) {
    buf[i] = hex[val & 0xF];
    val >>= 4;
  }
}

static void u64_to_dec(uint64_t val, char *buf) {
  if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
  char tmp[24]; int i = 0;
  while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
  int j = 0;
  while (i > 0) buf[j++] = tmp[--i];
  buf[j] = '\0';
}

// Create a directory under parent and return the new node.
static vfs_node_t *sysfs_mkdir(vfs_node_t *parent, const char *name) {
  vfs_node_t *dir = kmalloc(sizeof(vfs_node_t));
  if (!dir) return NULL;
  vfs_node_init(dir);
  strncpy(dir->name, name, 127);
  dir->flags = FS_DIRECTORY | FS_PERSISTENT;
  dir->mask  = 0555;
  ramfs_mount_on(dir);
  ramfs_mount_node(parent, dir);
  return dir;
}

// Create a read-only file under parent with the given content.
static void sysfs_mkfile(vfs_node_t *parent, const char *name,
                         const char *content) {
  vfs_node_t *f = kmalloc(sizeof(vfs_node_t));
  if (!f) return;
  vfs_node_init(f);
  strncpy(f->name, name, 127);
  f->flags = FS_FILE | FS_PERSISTENT;
  f->mask  = 0444;
  ramfs_mount_node(parent, f);
  uint32_t len = (uint32_t)strlen(content);
  vfs_write(f, 0, len, (uint8_t *)content);
}

// ── PCI bus population ────────────────────────────────────────────────────

static void sysfs_populate_pci(vfs_node_t *pci_devices_dir) {
  uint32_t count = pci_get_device_count();
  for (uint32_t i = 0; i < count; i++) {
    struct pci_device *dev = pci_get_device(i);
    if (!dev) continue;

    // Name: 0000:BB:SS.F
    char devname[16];
    devname[0] = '0'; devname[1] = '0'; devname[2] = '0'; devname[3] = '0';
    devname[4] = ':';
    char tmp[4];
    u32_to_hex(dev->bus,  tmp, 2); devname[5] = tmp[0]; devname[6] = tmp[1];
    devname[7] = ':';
    u32_to_hex(dev->slot, tmp, 2); devname[8] = tmp[0]; devname[9] = tmp[1];
    devname[10] = '.';
    devname[11] = '0' + (dev->func & 7);
    devname[12] = '\0';

    vfs_node_t *ddir = sysfs_mkdir(pci_devices_dir, devname);
    if (!ddir) continue;

    // vendor  (e.g. "0x8086\n")
    char vbuf[10];
    vbuf[0] = '0'; vbuf[1] = 'x';
    u32_to_hex(dev->vendor_id, vbuf + 2, 4);
    vbuf[6] = '\n'; vbuf[7] = '\0';
    sysfs_mkfile(ddir, "vendor", vbuf);

    // device
    char dbuf[10];
    dbuf[0] = '0'; dbuf[1] = 'x';
    u32_to_hex(dev->device_id, dbuf + 2, 4);
    dbuf[6] = '\n'; dbuf[7] = '\0';
    sysfs_mkfile(ddir, "device", dbuf);

    // class  (24-bit: class|subclass|progif as 0xCCSSPP)
    char cbuf[12];
    cbuf[0] = '0'; cbuf[1] = 'x';
    u32_to_hex(((uint32_t)dev->class_code << 16) |
               ((uint32_t)dev->subclass   <<  8) |
                (uint32_t)dev->prog_if,
               cbuf + 2, 6);
    cbuf[8] = '\n'; cbuf[9] = '\0';
    sysfs_mkfile(ddir, "class", cbuf);

    // irq
    char ibuf[8];
    u64_to_dec(dev->irq_line, ibuf);
    strcat(ibuf, "\n");
    sysfs_mkfile(ddir, "irq", ibuf);

    // resource (one line per BAR: start end flags)
    char rbuf[512];
    rbuf[0] = '\0';
    for (int b = 0; b < 6; b++) {
      if (dev->bar[b]) {
        char tmp2[12];
        strcat(rbuf, "0x");
        u32_to_hex(dev->bar[b], tmp2, 8);
        strcat(rbuf, tmp2);
        strcat(rbuf, " 0x");
        u32_to_hex(dev->bar[b] + 0xfff, tmp2, 8);
        strcat(rbuf, tmp2);
        strcat(rbuf, " 0x00000200\n");
      } else {
        strcat(rbuf, "0x00000000 0x00000000 0x00000000\n");
      }
    }
    sysfs_mkfile(ddir, "resource", rbuf);
  }
}

// ── Block class population ────────────────────────────────────────────────

static void sysfs_populate_block(vfs_node_t *block_class_dir) {
  int count = block_count();
  for (int i = 0; i < count; i++) {
    struct block_device *bd = block_get(i);
    if (!bd) continue;

    vfs_node_t *bdir = sysfs_mkdir(block_class_dir, bd->name);
    if (!bdir) continue;

    // dev: major:minor  (8:N for SCSI/ATA)
    char devbuf[16];
    devbuf[0] = '8'; devbuf[1] = ':';
    u64_to_dec(i, devbuf + 2);
    strcat(devbuf, "\n");
    sysfs_mkfile(bdir, "dev", devbuf);

    // size: total bytes
    char sbuf[32];
    uint64_t bytes = (uint64_t)bd->total_sectors *
                     (bd->sector_size ? bd->sector_size : 512);
    u64_to_dec(bytes, sbuf);
    strcat(sbuf, "\n");
    sysfs_mkfile(bdir, "size", sbuf);

    sysfs_mkfile(bdir, "removable", "0\n");
  }
}

// ── Net class population ──────────────────────────────────────────────────

static void sysfs_populate_net(vfs_node_t *net_class_dir) {
  if (!nic_is_present()) return;

  vfs_node_t *eth0 = sysfs_mkdir(net_class_dir, "eth0");
  if (!eth0) return;

  // MAC address
  const uint8_t *mac = nic_get_mac();
  char macbuf[20];
  if (mac) {
    const char *hex = "0123456789abcdef";
    int j = 0;
    for (int i = 0; i < 6; i++) {
      macbuf[j++] = hex[mac[i] >> 4];
      macbuf[j++] = hex[mac[i] & 0xF];
      macbuf[j++] = (i < 5) ? ':' : '\n';
    }
    macbuf[j] = '\0';
  } else {
    strcpy(macbuf, "00:00:00:00:00:00\n");
  }
  sysfs_mkfile(eth0, "address", macbuf);

  sysfs_mkfile(eth0, "operstate", nic_link_up() ? "up\n" : "down\n");
  sysfs_mkfile(eth0, "type",      "1\n"); // ARPHRD_ETHER
  sysfs_mkfile(eth0, "mtu",       "1500\n");
}

// ── CPU devices population ────────────────────────────────────────────────

static void sysfs_populate_cpus(vfs_node_t *cpu_dir) {
  uint32_t count = cpu_get_count();
  char name[16];
  for (uint32_t i = 0; i < count; i++) {
    strcpy(name, "cpu");
    u64_to_dec(i, name + 3);
    vfs_node_t *cdir = sysfs_mkdir(cpu_dir, name);
    if (!cdir) continue;
    sysfs_mkfile(cdir, "online", "1\n");
  }
  // Also add a 'possible' and 'present' file at the cpu/ level
  char cpumask[16];
  strcpy(cpumask, "0-");
  u64_to_dec(count - 1, cpumask + 2);
  strcat(cpumask, "\n");
  sysfs_mkfile(cpu_dir, "possible", cpumask);
  sysfs_mkfile(cpu_dir, "present",  cpumask);
}

// ── Main init ─────────────────────────────────────────────────────────────

void sysfs_init(void) {
  if (!fs_root) return;

  // Find or create the /sys directory on the root filesystem
  vfs_node_t *sys_dir = vfs_finddir(fs_root, "sys");
  if (!sys_dir && fs_root->mkdir) {
    fs_root->mkdir(fs_root, "sys", 0755);
    sys_dir = vfs_finddir(fs_root, "sys");
  }
  if (!sys_dir) {
    klog_puts("[SYSFS] Could not find/create /sys\n");
    return;
  }

  // Create a fresh ramfs root and mount it over /sys — same pattern as procfs
  vfs_node_t *sysfs_root = kmalloc(sizeof(vfs_node_t));
  if (!sysfs_root) return;
  vfs_node_init(sysfs_root);
  strcpy(sysfs_root->name, "sys");
  ramfs_mount_on(sysfs_root);
  vfs_mount(sys_dir, sysfs_root);

  // ── /sys/bus/pci/devices ─────────────────────────────────────────────
  vfs_node_t *bus_dir     = sysfs_mkdir(sysfs_root, "bus");
  vfs_node_t *pci_dir     = sysfs_mkdir(bus_dir,    "pci");
  vfs_node_t *pci_dev_dir = sysfs_mkdir(pci_dir,    "devices");
  sysfs_populate_pci(pci_dev_dir);

  // ── /sys/class ───────────────────────────────────────────────────────
  vfs_node_t *class_dir = sysfs_mkdir(sysfs_root, "class");

  // /sys/class/block
  vfs_node_t *block_class = sysfs_mkdir(class_dir, "block");
  sysfs_populate_block(block_class);

  // /sys/class/net
  vfs_node_t *net_class = sysfs_mkdir(class_dir, "net");
  sysfs_populate_net(net_class);

  // /sys/class/drm/card0
  vfs_node_t *drm_class = sysfs_mkdir(class_dir, "drm");
  vfs_node_t *card0_dir = sysfs_mkdir(drm_class, "card0");
  sysfs_mkfile(card0_dir, "dev",    "226:0\n");
  sysfs_mkfile(card0_dir, "uevent",
               "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\n"
               "DEVTYPE=drm_minor\nSUBSYSTEM=drm\n");

  // /sys/class/input/event0
  vfs_node_t *input_class = sysfs_mkdir(class_dir, "input");
  vfs_node_t *event0_dir  = sysfs_mkdir(input_class, "event0");
  sysfs_mkfile(event0_dir, "dev", "13:64\n");

  // ── /sys/devices/system/cpu ──────────────────────────────────────────
  vfs_node_t *devices_dir = sysfs_mkdir(sysfs_root, "devices");
  vfs_node_t *system_dir  = sysfs_mkdir(devices_dir, "system");
  vfs_node_t *cpu_dir     = sysfs_mkdir(system_dir,  "cpu");
  sysfs_populate_cpus(cpu_dir);

  // ── /sys/dev/block  /sys/dev/char ────────────────────────────────────
  vfs_node_t *dev_dir = sysfs_mkdir(sysfs_root, "dev");
  sysfs_mkdir(dev_dir, "block");
  vfs_node_t *char_dir = sysfs_mkdir(dev_dir, "char");
  // DRM char device
  vfs_node_t *drm_char = sysfs_mkdir(char_dir, "226:0");
  sysfs_mkfile(drm_char, "uevent",
               "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\nDEVTYPE=drm_minor\n");

  // ── /sys/kernel ──────────────────────────────────────────────────────
  vfs_node_t *kernel_dir = sysfs_mkdir(sysfs_root, "kernel");
  sysfs_mkfile(kernel_dir, "hostname", "ascentos\n");

  // ── /sys/power ───────────────────────────────────────────────────────
  vfs_node_t *power_dir = sysfs_mkdir(sysfs_root, "power");
  sysfs_mkfile(power_dir, "state", "mem\n");

  klog_puts("[OK] SysFS initialized at /sys\n");
}
