// lspci - list PCI devices
// Reads from /sys/bus/pci/devices/ (populated by sysfs_populate_pci in the kernel).
// Each subdirectory is a device named 0000:BB:SS.F and contains:
//   vendor, device, class, irq, resource
//
// Output format matches a simplified lspci -n style:
//   BB:SS.F  CCSSPP  VVVV:DDDD  [Class Description]

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PCI_DEVICES_PATH "/sys/bus/pci/devices"

// ── PCI class descriptions ────────────────────────────────────────────────

struct pci_class_entry {
  uint8_t     class_code;
  uint8_t     subclass;
  const char *desc;
};

static const struct pci_class_entry pci_classes[] = {
  { 0x00, 0x00, "Non-VGA unclassified device" },
  { 0x00, 0x01, "VGA-compatible unclassified device" },

  { 0x01, 0x00, "SCSI storage controller" },
  { 0x01, 0x01, "IDE interface" },
  { 0x01, 0x02, "Floppy disk controller" },
  { 0x01, 0x03, "IPI bus controller" },
  { 0x01, 0x04, "RAID bus controller" },
  { 0x01, 0x05, "ATA controller" },
  { 0x01, 0x06, "SATA controller" },
  { 0x01, 0x07, "Serial Attached SCSI controller" },
  { 0x01, 0x08, "Non-Volatile memory controller" },
  { 0x01, 0xFF, "Mass storage controller" },

  { 0x02, 0x00, "Ethernet controller" },
  { 0x02, 0x01, "Token ring network controller" },
  { 0x02, 0x02, "FDDI network controller" },
  { 0x02, 0x03, "ATM network controller" },
  { 0x02, 0x04, "ISDN controller" },
  { 0x02, 0x80, "Network controller" },

  { 0x03, 0x00, "VGA compatible controller" },
  { 0x03, 0x01, "XGA compatible controller" },
  { 0x03, 0x02, "3D controller" },
  { 0x03, 0x80, "Display controller" },

  { 0x04, 0x00, "Multimedia video controller" },
  { 0x04, 0x01, "Multimedia audio controller" },
  { 0x04, 0x02, "Computer telephony device" },
  { 0x04, 0x03, "Audio device" },
  { 0x04, 0x80, "Multimedia controller" },

  { 0x05, 0x00, "RAM memory" },
  { 0x05, 0x01, "FLASH memory" },
  { 0x05, 0x80, "Memory controller" },

  { 0x06, 0x00, "Host bridge" },
  { 0x06, 0x01, "ISA bridge" },
  { 0x06, 0x02, "EISA bridge" },
  { 0x06, 0x03, "MicroChannel bridge" },
  { 0x06, 0x04, "PCI bridge" },
  { 0x06, 0x05, "PCMCIA bridge" },
  { 0x06, 0x06, "NuBus bridge" },
  { 0x06, 0x07, "CardBus bridge" },
  { 0x06, 0x08, "RACEway bridge" },
  { 0x06, 0x09, "Semi-transparent PCI-to-PCI bridge" },
  { 0x06, 0x0A, "InfiniBand to PCI host bridge" },
  { 0x06, 0x80, "Bridge" },

  { 0x07, 0x00, "Serial controller" },
  { 0x07, 0x01, "Parallel controller" },
  { 0x07, 0x02, "Multiport serial controller" },
  { 0x07, 0x03, "Modem" },
  { 0x07, 0x80, "Communication controller" },

  { 0x08, 0x00, "PIC" },
  { 0x08, 0x01, "DMA controller" },
  { 0x08, 0x02, "Timer" },
  { 0x08, 0x03, "RTC" },
  { 0x08, 0x04, "PCI Hot-plug controller" },
  { 0x08, 0x05, "SD Host controller" },
  { 0x08, 0x06, "IOMMU" },
  { 0x08, 0x80, "System peripheral" },

  { 0x09, 0x00, "Keyboard controller" },
  { 0x09, 0x01, "Digitizer Pen" },
  { 0x09, 0x02, "Mouse controller" },
  { 0x09, 0x03, "Scanner controller" },
  { 0x09, 0x04, "Gameport controller" },
  { 0x09, 0x80, "Input device controller" },

  { 0x0A, 0x00, "Generic Docking Station" },
  { 0x0A, 0x80, "Docking station" },

  { 0x0B, 0x00, "386 Processor" },
  { 0x0B, 0x01, "486 Processor" },
  { 0x0B, 0x02, "Pentium Processor" },
  { 0x0B, 0x10, "Alpha Processor" },
  { 0x0B, 0x20, "Power PC Processor" },
  { 0x0B, 0x30, "MIPS Processor" },
  { 0x0B, 0x40, "Co-processor" },
  { 0x0B, 0x80, "Processor" },

  { 0x0C, 0x00, "FireWire (IEEE 1394)" },
  { 0x0C, 0x01, "ACCESS Bus" },
  { 0x0C, 0x02, "SSA" },
  { 0x0C, 0x03, "USB controller" },
  { 0x0C, 0x04, "Fibre Channel" },
  { 0x0C, 0x05, "SMBus" },
  { 0x0C, 0x06, "InfiniBand" },
  { 0x0C, 0x07, "IPMI Interface" },
  { 0x0C, 0x08, "SERCOS interface" },
  { 0x0C, 0x09, "CANBUS" },
  { 0x0C, 0x80, "Serial bus controller" },

  { 0x0D, 0x00, "IRDA controller" },
  { 0x0D, 0x01, "Consumer IR controller" },
  { 0x0D, 0x10, "RF controller" },
  { 0x0D, 0x11, "Bluetooth" },
  { 0x0D, 0x12, "Broadband" },
  { 0x0D, 0x20, "802.1a controller" },
  { 0x0D, 0x21, "802.1b controller" },
  { 0x0D, 0x80, "Wireless controller" },

  { 0x0E, 0x00, "I2O" },

  { 0x0F, 0x01, "Satellite TV controller" },
  { 0x0F, 0x02, "Satellite Audio communication controller" },
  { 0x0F, 0x03, "Satellite Voice communication controller" },
  { 0x0F, 0x04, "Satellite Data communication controller" },

  { 0x10, 0x00, "Network and computing encryption device" },
  { 0x10, 0x10, "Entertainment encryption device" },
  { 0x10, 0x80, "Encryption controller" },

  { 0x11, 0x00, "DPIO module" },
  { 0x11, 0x01, "Performance counters" },
  { 0x11, 0x10, "Communication synchronizer" },
  { 0x11, 0x20, "Signal processing management" },
  { 0x11, 0x80, "Signal processing controller" },

  { 0x12, 0x00, "Processing accelerators" },
  { 0x13, 0x00, "Non-Essential Instrumentation" },
  { 0xFF, 0xFF, NULL }, // sentinel
};

static const char *pci_class_desc(uint8_t cls, uint8_t sub) {
  // First pass: exact class+subclass match
  for (int i = 0; pci_classes[i].desc != NULL; i++) {
    if (pci_classes[i].class_code == cls &&
        pci_classes[i].subclass  == sub)
      return pci_classes[i].desc;
  }
  // Second pass: fallback to class+0x80 (generic label)
  for (int i = 0; pci_classes[i].desc != NULL; i++) {
    if (pci_classes[i].class_code == cls &&
        pci_classes[i].subclass  == 0x80)
      return pci_classes[i].desc;
  }
  return "Unknown device";
}

// ── Helpers ───────────────────────────────────────────────────────────────

// Read a sysfs attribute file and return the trimmed hex value, or -1.
static long read_sysfs_hex(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;

  char buf[32];
  int n = (int)read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = '\0';

  // Strip trailing whitespace / newlines
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                   buf[n - 1] == ' '))
    buf[--n] = '\0';

  // Parse: accepts "0x1234" or plain "1234"
  char *p = buf;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    p += 2;

  long val = 0;
  while (*p) {
    char c = *p++;
    int digit;
    if (c >= '0' && c <= '9')      digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else break;
    val = (val << 4) | digit;
  }
  return val;
}

// ── Main ──────────────────────────────────────────────────────────────────

int main(void) {
  DIR *root = opendir(PCI_DEVICES_PATH);
  if (!root) {
    fprintf(stderr, "lspci: cannot open %s\n", PCI_DEVICES_PATH);
    return 1;
  }

  struct dirent *ent;
  while ((ent = readdir(root)) != NULL) {
    // Skip . and ..
    if (ent->d_name[0] == '.')
      continue;

    // Build per-device sysfs path
    char devpath[256];
    snprintf(devpath, sizeof(devpath), "%s/%s", PCI_DEVICES_PATH, ent->d_name);

    // Read vendor, device, class
    char attr[320];
    snprintf(attr, sizeof(attr), "%s/vendor", devpath);
    long vendor = read_sysfs_hex(attr);

    snprintf(attr, sizeof(attr), "%s/device", devpath);
    long device = read_sysfs_hex(attr);

    snprintf(attr, sizeof(attr), "%s/class", devpath);
    long cls_raw = read_sysfs_hex(attr);  // 0xCCSSPP (24-bit)

    if (vendor < 0 || device < 0 || cls_raw < 0)
      continue;

    uint8_t cls = (uint8_t)((cls_raw >> 16) & 0xFF);
    uint8_t sub = (uint8_t)((cls_raw >>  8) & 0xFF);

    // ent->d_name is "0000:BB:SS.F" — print from the BB: part (skip "0000:")
    const char *addr = ent->d_name;
    if (strlen(addr) > 5 && addr[4] == ':')
      addr += 5; // skip "0000:"

    printf("%s  %02x%02x  %04lx:%04lx  %s\n",
           addr, cls, sub, vendor, device, pci_class_desc(cls, sub));
  }

  closedir(root);
  return 0;
}
