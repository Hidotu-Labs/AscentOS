#include "drivers/storage/ahci.h"
#include "apic/lapic_timer.h"
#include "console/console.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "drivers/storage/block.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "drivers/manager/device.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATA_CMD_READ_DMA_EX 0x25
#define ATA_CMD_WRITE_DMA_EX 0x35
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA

// Private structures

struct ahci_drive {
  ahci_port_t *port;
  bool present;
  uint64_t total_sectors;
  char model[41];
  bool failed; /* Fail-stop after an uncertain command; never risk a later write. */
  struct block_device blkdev;
  spinlock_t lock; // Protect access to this port's command slots
};

#define AHCI_TFD_BSY (1u << 7)
#define AHCI_TFD_DRQ (1u << 3)
#define AHCI_PXIS_TFES (1u << 30)
#define AHCI_MAX_SECTORS_PER_CMD 8192u /* one PRDT entry: at most 4 MiB */

static ahci_hba_mem_t *hba;
static struct ahci_drive ahci_drives[32];
static int ahci_drive_count = 0;

// Helpers

static void print_uint64(uint64_t num) {
  if (num == 0) {
    console_putchar('0');
    return;
  }
  char buf[20];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    console_putchar(buf[--i]);
  }
}

// AHCI Control

static int find_cmdslot(ahci_port_t *port) {
  // If not set, bit isn't set in SACT and CI
  uint32_t slots = (port->sact | port->ci);
  // Hardcoded 32 slots limit
  for (int i = 0; i < 32; i++) {
    if ((slots & (1 << i)) == 0)
      return i;
  }
  return -1;
}

static void stop_cmd(ahci_port_t *port) {
  // Clear ST (Start)
  port->cmd &= ~AHCI_CMD_ST;
  // Clear FRE (FIS Receive Enable)
  port->cmd &= ~AHCI_CMD_FRE;

  // Wait until FR (FIS Receive Running) and CR (Command List Running) are
  // cleared
  while (1) {
    if (port->cmd & AHCI_CMD_FR)
      continue;
    if (port->cmd & AHCI_CMD_CR)
      continue;
    break;
  }
}

static void start_cmd(ahci_port_t *port) {
  // Wait until CR (Command List Running) is cleared
  while (port->cmd & AHCI_CMD_CR)
    ;

  // Set FRE (FIS Receive Enable) and ST (Start)
  port->cmd |= AHCI_CMD_FRE;
  port->cmd |= AHCI_CMD_ST;
}

/* Stop DMA before releasing memory named by a failed command table. */
static bool quiesce_port(ahci_port_t *port) {
  port->cmd &= ~AHCI_CMD_ST;
  uint64_t deadline = lapic_timer_get_ms() + 1000;
  while (port->cmd & AHCI_CMD_CR) {
    if (lapic_timer_get_ms() >= deadline)
      return false;
    __asm__ volatile("pause");
  }
  port->cmd &= ~AHCI_CMD_FRE;
  deadline = lapic_timer_get_ms() + 1000;
  while (port->cmd & AHCI_CMD_FR) {
    if (lapic_timer_get_ms() >= deadline)
      return false;
    __asm__ volatile("pause");
  }
  return true;
}

static void port_rebase(ahci_port_t *port) {
  stop_cmd(port);

  // Command list offset: 1K per port
  void *phys_clb = pmm_alloc(); // alloc 1 page (4K) for simplicity
  memset((void *)((uint64_t)phys_clb + pmm_get_hhdm_offset()), 0, 1024);
  port->clb = (uint32_t)(uint64_t)phys_clb;
  port->clbu = (uint32_t)((uint64_t)phys_clb >> 32);

  // FIS offset: 256 bytes per port - just use the second KB of the same page
  uint64_t phys_fb = (uint64_t)phys_clb + 1024;
  memset((void *)(phys_fb + pmm_get_hhdm_offset()), 0, 256);
  port->fb = (uint32_t)phys_fb;
  port->fbu = (uint32_t)(phys_fb >> 32);

  // Command table offset: 256 bytes per command table, 32 commands
  // We need 256 * 32 = 8K per port. Alloc 2 pages.
  void *phys_ctba_base = pmm_alloc_blocks(2);
  memset((void *)((uint64_t)phys_ctba_base + pmm_get_hhdm_offset()), 0, 8192);

  ahci_command_header_t *cmdheader =
      (ahci_command_header_t *)((uint64_t)phys_clb + pmm_get_hhdm_offset());
  for (int i = 0; i < 32; i++) {
    cmdheader[i].prdtl =
        8; // Max 8 PRDT entries per command slot for our static struct size
    uint64_t phys_ctba = (uint64_t)phys_ctba_base + (256 * i);
    cmdheader[i].ctba = (uint32_t)phys_ctba;
    cmdheader[i].ctbau = (uint32_t)(phys_ctba >> 32);
  }

  start_cmd(port);
}

// AHCI Command IO

static int ahci_io(ahci_port_t *port, uint64_t lba, uint32_t count, void *buf,
                   int is_write) {
  // Find the drive associated with this port to get its lock
  struct ahci_drive *drive = NULL;
  for (int i = 0; i < 32; i++) {
    if (ahci_drives[i].port == port) {
      drive = &ahci_drives[i];
      break;
    }
  }

  if (drive)
    spinlock_acquire(&drive->lock);

  if (!drive || drive->failed || !buf || count == 0 ||
      count > AHCI_MAX_SECTORS_PER_CMD || lba >= drive->total_sectors ||
      (uint64_t)count > drive->total_sectors - lba ||
      lba > 0x0000FFFFFFFFFFFFULL) {
    if (drive)
      spinlock_release(&drive->lock);
    return -1;
  }

  // Clear pending interrupts
  port->is = port->is;

  int slot = find_cmdslot(port);
  if (slot == -1) {
    if (drive)
      spinlock_release(&drive->lock);
    return -1;
  }

  // Get HHDM VA of command list
  uint64_t clb_addr = ((uint64_t)port->clbu << 32) | port->clb;
  ahci_command_header_t *cmdheader =
      (ahci_command_header_t *)(clb_addr + pmm_get_hhdm_offset());

  // Command size (5 dwords in FIS)
  cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
  cmdheader[slot].w = is_write ? 1 : 0;

  // One PRDT entry per page
  cmdheader[slot].prdtl = 1;

  // Get HHDM VA of command table
  uint64_t ctba_addr =
      ((uint64_t)cmdheader[slot].ctbau << 32) | cmdheader[slot].ctba;
  ahci_command_table_t *cmdtbl =
      (ahci_command_table_t *)(ctba_addr + pmm_get_hhdm_offset());
  memset(cmdtbl, 0, sizeof(ahci_command_table_t));

  // Use a DMA bounce buffer in physical memory. The syscall/file layer may
  // pass user virtual addresses which are NOT in HHDM and are not guaranteed
  // physically contiguous.
  uint64_t bytes = (uint64_t)count * 512;
  uint64_t pages = (bytes + 4095) / 4096;
  void *bounce_phys = pmm_alloc_blocks(pages);
  if (!bounce_phys) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " AHCI OOM: bounce buffer alloc failed\n");
    spinlock_release(&drive->lock);
    return -1;
  }
  void *bounce_virt = (void *)((uint64_t)bounce_phys + pmm_get_hhdm_offset());

  if (is_write) {
    memcpy(bounce_virt, buf, (size_t)bytes);
  } else {
    memset(bounce_virt, 0, (size_t)bytes);
  }

  cmdtbl->prdt_entry[0].dba = (uint32_t)(uint64_t)bounce_phys;
  cmdtbl->prdt_entry[0].dbau = (uint32_t)((uint64_t)bounce_phys >> 32);
  cmdtbl->prdt_entry[0].dbc = bytes - 1; // Byte count - 1
  cmdtbl->prdt_entry[0].i = 1;           // Interrupt on completion

  // Setup FIS
  fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)(&cmdtbl->cfis);
  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1; // Command
  cmdfis->command = is_write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;

  cmdfis->lba0 = (uint8_t)(lba & 0xFF);
  cmdfis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
  cmdfis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
  cmdfis->device = 1 << 6; // LBA mode

  cmdfis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
  cmdfis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
  cmdfis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

  cmdfis->countl = count & 0xFF;
  cmdfis->counth = (count >> 8) & 0xFF;

  // Issue command, but never let a wedged controller freeze the kernel.
  uint64_t deadline = lapic_timer_get_ms() + 5000;
  while (port->tfd & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) {
    if (lapic_timer_get_ms() >= deadline) {
      console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                   " AHCI timeout waiting for BSY/DRQ to clear\n");
      drive->failed = true;
      pmm_free_blocks(bounce_phys, pages);
      if (drive) spinlock_release(&drive->lock);
      return -1;
    }
    __asm__ volatile("pause");
  }

  port->ci = 1 << slot;
  deadline = lapic_timer_get_ms() + 5000;

  // Wait for completion
  while (1) {
    // If the command issue bit clears, it's done
    if ((port->ci & (1 << slot)) == 0)
      break;
    if (port->is & AHCI_PXIS_TFES) {
      console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " AHCI Disk Error during wait: IS=");
      break;
    }
    if (lapic_timer_get_ms() >= deadline) {
      console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                   " AHCI command timeout: CI=");
      print_uint64(port->ci);
      console_puts(" TFD=");
      print_uint64(port->tfd);
      console_puts(" IS=");
      print_uint64(port->is);
      console_puts("\n");
      break;
    }
    __asm__ volatile("pause");
  }

  /* CI may clear in the same MMIO update that reports TFES. */
  if ((port->ci & (1u << slot)) != 0 ||
      (port->is & AHCI_PXIS_TFES) != 0 || (port->tfd & 0x01) != 0) {
    drive->failed = true;
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                 " AHCI port disabled after an uncertain command\n");
    /* Never reuse memory while the HBA may still own it. */
    if (quiesce_port(port))
      pmm_free_blocks(bounce_phys, pages);
    spinlock_release(&drive->lock);
    return -1;
  }

  if (!is_write) {
    memcpy(buf, bounce_virt, (size_t)bytes);
  }
  pmm_free_blocks(bounce_phys, pages);

  if (drive)
    spinlock_release(&drive->lock);
  return 0;
}

static int ahci_read(struct block_device *dev, uint64_t lba, uint32_t count,
                     void *buf) {
  struct ahci_drive *drive = (struct ahci_drive *)dev->driver_data;
  return ahci_io(drive->port, lba, count, buf, 0);
}

static int ahci_write(struct block_device *dev, uint64_t lba, uint32_t count,
                      const void *buf) {
  struct ahci_drive *drive = (struct ahci_drive *)dev->driver_data;
  // Cast away const for our naive physical address calculation
  return ahci_io(drive->port, lba, count, (void *)buf, 1);
}

static int ahci_flush(struct block_device *dev) {
  struct ahci_drive *drive = (struct ahci_drive *)dev->driver_data;
  if (!drive || drive->failed)
    return -1;

  spinlock_acquire(&drive->lock);
  ahci_port_t *port = drive->port;
  port->is = port->is;
  int slot = find_cmdslot(port);
  if (slot < 0) {
    spinlock_release(&drive->lock);
    return -1;
  }

  uint64_t clb_addr = ((uint64_t)port->clbu << 32) | port->clb;
  ahci_command_header_t *header =
      (ahci_command_header_t *)(clb_addr + pmm_get_hhdm_offset());
  header[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
  header[slot].w = 0;
  header[slot].prdtl = 0;

  uint64_t ctba = ((uint64_t)header[slot].ctbau << 32) | header[slot].ctba;
  ahci_command_table_t *table =
      (ahci_command_table_t *)(ctba + pmm_get_hhdm_offset());
  memset(table, 0, sizeof(*table));
  fis_reg_h2d_t *fis = (fis_reg_h2d_t *)&table->cfis;
  fis->fis_type = FIS_TYPE_REG_H2D;
  fis->c = 1;
  fis->command = ATA_CMD_FLUSH_CACHE_EXT;

  uint64_t deadline = lapic_timer_get_ms() + 5000;
  while (port->tfd & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) {
    if (lapic_timer_get_ms() >= deadline) {
      spinlock_release(&drive->lock);
      return -1;
    }
    __asm__ volatile("pause");
  }

  port->ci = 1u << slot;
  while (port->ci & (1u << slot)) {
    if ((port->is & AHCI_PXIS_TFES) ||
        lapic_timer_get_ms() >= deadline) {
      drive->failed = true;
      spinlock_release(&drive->lock);
      return -1;
    }
    __asm__ volatile("pause");
  }

  int result = ((port->is & AHCI_PXIS_TFES) || (port->tfd & 0x01)) ? -1 : 0;
  if (result)
    drive->failed = true;
  spinlock_release(&drive->lock);
  return result;
}

// Identify & Setup

static bool ahci_identify(ahci_port_t *port, struct ahci_drive *drive) {
  int slot = find_cmdslot(port);
  if (slot == -1)
    return false;

  // Use page allocator for temp buffer (must be physical!)
  void *phys_buf = pmm_alloc();
  void *virt_buf = (void *)((uint64_t)phys_buf + pmm_get_hhdm_offset());
  memset(virt_buf, 0, 4096);

  uint64_t clb_addr = ((uint64_t)port->clbu << 32) | port->clb;
  ahci_command_header_t *cmdheader =
      (ahci_command_header_t *)(clb_addr + pmm_get_hhdm_offset());

  cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
  cmdheader[slot].w = 0;
  cmdheader[slot].prdtl = 1;

  uint64_t ctba_addr =
      ((uint64_t)cmdheader[slot].ctbau << 32) | cmdheader[slot].ctba;
  ahci_command_table_t *cmdtbl =
      (ahci_command_table_t *)(ctba_addr + pmm_get_hhdm_offset());
  memset(cmdtbl, 0, sizeof(ahci_command_table_t));

  cmdtbl->prdt_entry[0].dba = (uint32_t)(uint64_t)phys_buf;
  cmdtbl->prdt_entry[0].dbau = (uint32_t)((uint64_t)phys_buf >> 32);
  cmdtbl->prdt_entry[0].dbc = 511; // 512 bytes
  cmdtbl->prdt_entry[0].i = 1;

  fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)(&cmdtbl->cfis);
  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1;
  cmdfis->command = ATA_CMD_IDENTIFY;

  while (port->tfd & (0x80 | 0x08))
    ;

  port->ci = 1 << slot;

  while (1) {
    if ((port->ci & (1 << slot)) == 0)
      break;
    if (port->is & (1 << 30))
      return false;
  }

  uint16_t *identify_data = (uint16_t *)virt_buf;

  uint64_t lba48_sectors = (uint64_t)identify_data[100] |
                           ((uint64_t)identify_data[101] << 16) |
                           ((uint64_t)identify_data[102] << 32) |
                           ((uint64_t)identify_data[103] << 48);

  uint32_t lba28_sectors =
      (uint32_t)identify_data[60] | ((uint32_t)identify_data[61] << 16);

  drive->total_sectors = lba48_sectors ? lba48_sectors : lba28_sectors;

  for (int i = 0; i < 20; i++) {
    drive->model[i * 2] = (char)(identify_data[27 + i] >> 8);
    drive->model[i * 2 + 1] = (char)(identify_data[27 + i] & 0xFF);
  }
  drive->model[40] = '\0';

  for (int i = 39; i >= 0; i--) {
    if (drive->model[i] == ' ')
      drive->model[i] = '\0';
    else
      break;
  }

  pmm_free(phys_buf);
  return true;
}

static void probe_port(ahci_port_t *port, int portno) {
  uint32_t ssts = port->ssts;

  uint8_t ipm = (ssts >> 8) & 0x0F;
  uint8_t det = ssts & 0x0F;

  if (det != AHCI_PORT_DET_PRESENT || ipm != AHCI_PORT_IPM_ACTIVE)
    return; // Device not present or not active

  // Check signature
  if (port->sig == SATA_SIG_ATAPI) {
    console_puts("     Port ");
    console_putchar('0' + portno);
    console_puts(": ATAPI drive found. Skipping.\n");
  } else if (port->sig == SATA_SIG_SEMB) {
    // Enclosure management
  } else if (port->sig == SATA_SIG_PM) {
    // Port multiplier
  } else if (port->sig == SATA_SIG_ATA) {
    console_puts("     Port ");
    console_putchar('0' + portno);
    console_puts(": SATA drive found.\n");
    port_rebase(port);

    struct ahci_drive *drive = &ahci_drives[ahci_drive_count];
    drive->port = port;
    drive->lock = (spinlock_t)SPINLOCK_INIT;

    if (ahci_identify(port, drive)) {
      drive->present = true;
      uint64_t size_mb = (drive->total_sectors * 512) / (1024 * 1024);

      console_puts("     sata");
      console_putchar('0' + ahci_drive_count);
      console_puts(": ");
      console_puts(drive->model);
      console_puts(" (");
      print_uint64(size_mb);
      console_puts(" MB, ");
      print_uint64(drive->total_sectors);
      console_puts(" sectors)\n");

      struct block_device *blk = &drive->blkdev;
      blk->name[0] = 's';
      blk->name[1] = 'a';
      blk->name[2] = 't';
      blk->name[3] = 'a';
      blk->name[4] = '0' + ahci_drive_count;
      blk->name[5] = '\0';
      blk->sector_size = 512;
      blk->total_sectors = drive->total_sectors;
      blk->read_sectors = ahci_read;
      blk->write_sectors = ahci_write;
      blk->flush = ahci_flush;
      blk->driver_data = drive;

      block_register(blk);
      ahci_drive_count++;
    }
  }
}

// Initialization

static int ahci_probe(struct device *dev) {
  uint32_t abar = 0;
  // Get ABAR from BAR5
  // We should ideally have BARs in dev->resources, but for now we'll do legacy PCI read
  struct pci_device *pci_dev = pci_find_device_by_id(dev->vendor_id, dev->device_id);
  if (!pci_dev) return -1;
  
  abar = pci_dev->bar[5];
  if (abar == 0) return -1;

  uint64_t phys_abar = abar & 0xFFFFFFF0;
  uint64_t virt_abar = phys_abar + pmm_get_hhdm_offset();
  
  // The kernel accesses PCI MMIO through the existing HHDM mapping. Do not
  // reinstall it here: remapping during SMP early boot requires a global TLB
  // shootdown and can deadlock idle APs.

  hba = (ahci_hba_mem_t *)virt_abar;
  hba->ghc |= (1 << 31); // AHCI Enable

  uint32_t pi = hba->pi;
  for (int i = 0; i < 32; i++) {
    if (pi & (1 << i)) {
      probe_port(&hba->ports[i], i);
    }
  }
  return 0;
}

static struct device_id ahci_ids[] = {
    { .type = ID_PCI, .pci = { .match_class = true, .class = 0x01, .subclass = 0x06 } }
};

static struct driver ahci_driver = {
    .name = "ahci",
    .ids = ahci_ids,
    .id_count = 1,
    .probe = ahci_probe
};

int ahci_init(void) {
  ahci_drive_count = 0;
  dm_register_driver(&ahci_driver);

  if (ahci_drive_count == 0) {
    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " No SATA drives successfully initialized.\n");
  } else {
    console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " AHCI: ");
    print_uint64(ahci_drive_count);
    console_puts(" drive(s) registered.\n");
  }
  return ahci_drive_count;
}
