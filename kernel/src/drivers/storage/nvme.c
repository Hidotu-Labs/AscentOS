#include "nvme.h"
#include "../../console/klog.h"
#include "../../cpu/irq.h"
#include "../../cpu/isr.h"
#include "../../lib/string.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../manager/device.h"
#include "../pci/pci.h"
#include "block.h"
#include <stddef.h>

#define NVME_MAX_CONTROLLERS 4

typedef struct msix_table_entry {
  uint32_t addr_lo;
  uint32_t addr_hi;
  uint32_t data;
  uint32_t vec_ctrl;
} msix_table_entry_t;

// Forward declarations
static int nvme_init_controller(struct nvme_controller *nvme);
static int nvme_identify(struct nvme_controller *nvme);
static int nvme_create_io_queues(struct nvme_controller *nvme);
static int nvme_block_read(struct block_device *dev, uint64_t lba,
                           uint32_t count, void *buf);
static int nvme_block_write(struct block_device *dev, uint64_t lba,
                            uint32_t count, const void *buf);
static void nvme_irq_handler(struct registers *regs);
static int nvme_setup_msix(struct nvme_controller *nvme,
                           struct pci_device *pdev);

static struct nvme_controller controllers[NVME_MAX_CONTROLLERS];
static int nvme_count = 0;

static int nvme_probe(struct device *dev) {
  if (nvme_count >= NVME_MAX_CONTROLLERS)
    return -1;

  klog_puts("[NVME] Probing controller...\n");

  // Find the PCI device for this generic device manager node
  struct pci_device *pdev =
      pci_find_device_by_id(dev->vendor_id, dev->device_id);
  if (!pdev) {
    klog_puts("[ERR] NVMe: Could not find associated PCI device.\n");
    return -1;
  }

  klog_puts("[NVME] PCI ID: ");
  klog_hex32(pdev->vendor_id);
  klog_puts(":");
  klog_hex32(pdev->device_id);
  klog_puts("\n");

  struct nvme_controller *nvme = &controllers[nvme_count++];
  nvme->dev = dev;
  dev->driver_data = nvme;

  // 1. Enable Bus Mastering and Memory Space
  uint32_t pci_cmd = pci_config_read32(pdev->bus, pdev->slot, pdev->func, 0x04);
  pci_config_write32(pdev->bus, pdev->slot, pdev->func, 0x04, pci_cmd | 0x06);

  // 2. Map BAR0 (Controller Registers)
  // NVMe BAR0 is 64-bit (BAR0 + BAR1)
  uint64_t phys_base = pdev->bar[0] & 0xFFFFFFF0;
  if ((pdev->bar[0] & 0x7) == 0x4) { // 64-bit BAR
    phys_base |= ((uint64_t)pdev->bar[1] << 32);
  }
  uintptr_t virt_base = phys_base + pmm_get_hhdm_offset();

  // Map first two pages (8KB) - minimum for registers.
  // Spec usually wants more for doorbells depending on CAP.STRIDE
  vmm_map_page(vmm_get_active_pml4(), virt_base, phys_base,
               PAGE_FLAG_RW | PAGE_FLAG_PRESENT);
  vmm_map_page(vmm_get_active_pml4(), virt_base + 0x1000, phys_base + 0x1000,
               PAGE_FLAG_RW | PAGE_FLAG_PRESENT);
  // Doorbell registers usually start at 0x1000, so we need at least 2 pages.
  // However, depending on the number of queues, we might need significantly


  nvme->regs = (nvme_regs_t *)virt_base;
  nvme->present = true;

  // --- Diagnostic Start ---
  uint32_t *raw = (uint32_t *)virt_base;
  klog_puts("[NVME] Raw BAR0: ");
  klog_hex32(raw[0]);
  klog_puts(" "); // CAP Low
  klog_hex32(raw[1]);
  klog_puts(" "); // CAP High
  klog_hex32(raw[2]);
  klog_puts(" "); // VS
  klog_hex32(raw[3]);
  klog_puts("\n");
  // --- Diagnostic End ---


  uint32_t version = nvme->regs->vs;
  uint64_t cap = nvme->regs->cap;

  klog_puts("[NVME] Version: ");
  klog_uint64((version >> 16) & 0xFFFF);
  klog_puts(".");
  klog_uint64((version >> 8) & 0xFF);
  klog_puts(".");
  klog_uint64(version & 0xFF);
  klog_puts("\n");

  klog_puts("[NVME] Cap: ");
  klog_uint64(cap);
  klog_puts("\n");

  nvme->db_stride = 1 << (2 + ((cap >> 32) & 0xF));

  klog_puts("[NVME] Doorbell Stride: ");
  klog_uint64(nvme->db_stride);
  klog_puts(" bytes\n");


  if (nvme_init_controller(nvme) != 0)
    return -1;
  if (nvme_identify(nvme) != 0)
    return -1;
  if (nvme_create_io_queues(nvme) != 0)
    return -1;


  nvme->bdev.driver_data = nvme;
  nvme->bdev.read_sectors = nvme_block_read;
  nvme->bdev.write_sectors = nvme_block_write;
  nvme->bdev.sector_size = 512;
  nvme->bdev.total_sectors = nvme->capacity_sectors;

  // Name it nvmeX
  memcpy(nvme->bdev.name, "nvme", 4);
  nvme->bdev.name[4] = '0' + (nvme_count - 1);
  nvme->bdev.name[5] = '\0';

  block_register(&nvme->bdev);

  return 0;
}

static bool nvme_wait_ready(struct nvme_controller *nvme, bool ready) {
  int timeout = 100000;
  while (timeout--) {
    bool rdy = (nvme->regs->csts & 1); // RDY bit
    if (rdy == ready)
      return true;
    for (int i = 0; i < 1000; i++)
      __asm__ volatile("pause");
  }
  return false;
}

static int nvme_init_controller(struct nvme_controller *nvme) {
  klog_puts("[NVME] Starting controller init...\n");

  // 1. Disable the controller
  if (nvme->regs->cc & 1) {
    klog_puts("[NVME]   Disabling controller...\n");
    nvme->regs->cc &= ~1; // EN = 0
  }

  if (!nvme_wait_ready(nvme, false)) {
    klog_puts("[ERR] NVMe: Controller reset timeout (RDY stayed 1).\n");
    return -1;
  }
  klog_puts("[NVME]   Controller disabled/reset.\n");


  struct pci_device *pdev =
      pci_find_device_by_id(nvme->dev->vendor_id, nvme->dev->device_id);
  if (pdev)
    nvme_setup_msix(nvme, pdev);

  // Clear interrupt masks
  nvme->regs->intmc = 0xFFFFFFFF;

  // 3. Allocate Admin Queues
  klog_puts("[NVME]   Allocating Admin Queues...\n");
  uint64_t asq_phys, acq_phys;
  nvme->admin_sq = pmm_alloc();
  asq_phys = (uint64_t)nvme->admin_sq;
  nvme->admin_cq = pmm_alloc();
  acq_phys = (uint64_t)nvme->admin_cq;

  memset((void *)(asq_phys + pmm_get_hhdm_offset()), 0, 4096);
  memset((void *)(acq_phys + pmm_get_hhdm_offset()), 0, 4096);

  // 3. Configure Admin Queue Attributes
  klog_puts("[NVME]   Configuring Admin Queues...\n");
  uint32_t aqa = (255 << 16) | 63;
  nvme->regs->aqa = aqa;
  nvme->regs->asq = asq_phys;
  nvme->regs->acq = acq_phys;

  // 4. Set Controller Configuration
  klog_puts("[NVME]   Enabling controller...\n");
  uint32_t cc = (6 << 16) | (4 << 20);
  nvme->regs->cc = cc | 1; // Set EN = 1

  if (!nvme_wait_ready(nvme, true)) {
    klog_puts("[ERR] NVMe: Controller enable timeout (RDY stayed 0).\n");
    return -1;
  }
  klog_puts("[NVME]   Controller READY.\n");

  nvme->admin_sq_tail = 0;
  nvme->admin_cq_head = 0;

  return 0;
}

static int nvme_submit_admin_cmd(struct nvme_controller *nvme, nvme_cmd_t *cmd,
                                 nvme_completion_t *res) {
  uintptr_t sq_virt = (uintptr_t)nvme->admin_sq + pmm_get_hhdm_offset();
  uintptr_t cq_virt = (uintptr_t)nvme->admin_cq + pmm_get_hhdm_offset();

  nvme_cmd_t *sq = (nvme_cmd_t *)sq_virt;
  nvme_completion_t *cq = (nvme_completion_t *)cq_virt;

  // 1. Copy command to SQ tail
  memcpy(&sq[nvme->admin_sq_tail], cmd, sizeof(nvme_cmd_t));

  // 2. Advance tail and ring doorbell
  nvme->admin_sq_tail = (nvme->admin_sq_tail + 1) % 64;
  volatile uint32_t *sq_db =
      (volatile uint32_t *)((uintptr_t)nvme->regs + 0x1000);
  *sq_db = nvme->admin_sq_tail;

  // 3. Wait for completion
  int timeout = 100000;
  while (timeout--) {
    // Completion entries have a "Phase Tag" (bit 0 of status) that flips
    // We zeroed the memory, so the first round of completions will have Phase=1
    uint16_t status = cq[nvme->admin_cq_head].status;
    if ((status & 0x1) == 1) { // Simplified Phase check: our init zeroed it
      if (res)
        memcpy(res, &cq[nvme->admin_cq_head], sizeof(nvme_completion_t));

      // 4. Advance head and ring doorbell
      nvme->admin_cq_head = (nvme->admin_cq_head + 1) % 256;
      volatile uint32_t *cq_db =
          (volatile uint32_t *)((uintptr_t)nvme->regs + 0x1000 +
                                nvme->db_stride);
      *cq_db = nvme->admin_cq_head;

      return 0;
    }
    for (int i = 0; i < 100; i++)
      __asm__ volatile("pause");
  }

  return -1;
}

static int nvme_identify(struct nvme_controller *nvme) {
  void *phys_buf = pmm_alloc();
  void *virt_buf = (void *)((uintptr_t)phys_buf + pmm_get_hhdm_offset());
  memset(virt_buf, 0, 4096);

  // Identify Controller
  nvme_cmd_t cmd = {0};
  cmd.cd0 = 0x06; // Opcode: Identify
  cmd.nsid = 0;
  cmd.prp1 = (uint64_t)phys_buf;
  cmd.cd10 = 1; // CNS: Identify Controller

  if (nvme_submit_admin_cmd(nvme, &cmd, NULL) != 0) {
    klog_puts("[ERR] NVMe: Identify Controller failed.\n");
    pmm_free(phys_buf);
    return -1;
  }

  // Extract Model (offset 24, 40 bytes) and Serial (offset 4, 20 bytes)
  char model[41];
  char serial[21];
  memcpy(model, (char *)virt_buf + 24, 40);
  model[40] = '\0';
  memcpy(serial, (char *)virt_buf + 4, 20);
  serial[20] = '\0';

  klog_puts("[NVME] Model:  ");
  klog_puts(model);
  klog_puts("\n");
  klog_puts("[NVME] Serial: ");
  klog_puts(serial);
  klog_puts("\n");

  // Identify Namespace 1
  memset(virt_buf, 0, 4096);
  cmd.nsid = 1;
  cmd.cd10 = 0; // CNS: Identify Namespace

  if (nvme_submit_admin_cmd(nvme, &cmd, NULL) != 0) {
    klog_puts("[ERR] NVMe: Identify Namespace failed.\n");
    pmm_free(phys_buf);
    return -1;
  }

  uint64_t nsze = *(uint64_t *)virt_buf;
  nvme->capacity_sectors = nsze;
  uint8_t flbas = *((uint8_t *)virt_buf + 26);
  uint8_t lbads = *((uint8_t *)virt_buf + 128 + (flbas & 0xF) * 4 + 2);
  uint32_t sector_size = 1 << lbads;

  klog_puts("[NVME] Size:   ");
  klog_uint64((nsze * sector_size) / (1024 * 1024));
  klog_puts(" MB (");
  klog_uint64(nsze);
  klog_puts(" sectors of ");
  klog_uint64(sector_size);
  klog_puts(" bytes)\n");

  pmm_free(phys_buf);
  return 0;
}

static int nvme_create_io_queues(struct nvme_controller *nvme) {
  klog_puts("[NVME]   Creating IO Queues...\n");

  // 1. Allocate Memory
  uint64_t sq_phys, cq_phys;
  nvme->io_sq = pmm_alloc();
  sq_phys = (uint64_t)nvme->io_sq;
  nvme->io_cq = pmm_alloc();
  cq_phys = (uint64_t)nvme->io_cq;

  memset((void *)(sq_phys + pmm_get_hhdm_offset()), 0, 4096);
  memset((void *)(cq_phys + pmm_get_hhdm_offset()), 0, 4096);

  // 2. Create Completion Queue (Opcode 0x05)
  nvme_cmd_t cmd = {0};
  cmd.cd0 = 0x05;
  cmd.prp1 = cq_phys;
  cmd.cd10 = (255 << 16) | 1; // QID=1, Size=256 (255)
  cmd.cd11 = 1;               // PC=1 (Physically Contiguous)

  if (nvme_submit_admin_cmd(nvme, &cmd, NULL) != 0) {
    klog_puts("[ERR] NVMe: Create IO CQ failed.\n");
    return -1;
  }

  // 3. Create Submission Queue (Opcode 0x01)
  memset(&cmd, 0, sizeof(nvme_cmd_t));
  cmd.cd0 = 0x01;
  cmd.prp1 = sq_phys;
  cmd.cd10 = (63 << 16) | 1; // QID=1, Size=64 (63)
  cmd.cd11 = (1 << 16) | 1;  // CQID=1, PC=1

  if (nvme_submit_admin_cmd(nvme, &cmd, NULL) != 0) {
    klog_puts("[ERR] NVMe: Create IO SQ failed.\n");
    return -1;
  }

  nvme->io_sq_tail = 0;
  nvme->io_cq_head = 0;

  return 0;
}

static int nvme_submit_io_cmd(struct nvme_controller *nvme, nvme_cmd_t *cmd,
                              nvme_completion_t *res) {
  uintptr_t sq_virt = (uintptr_t)nvme->io_sq + pmm_get_hhdm_offset();
  uintptr_t cq_virt = (uintptr_t)nvme->io_cq + pmm_get_hhdm_offset();

  nvme_cmd_t *sq = (nvme_cmd_t *)sq_virt;
  nvme_completion_t *cq = (nvme_completion_t *)cq_virt;

  memcpy(&sq[nvme->io_sq_tail], cmd, sizeof(nvme_cmd_t));

  nvme->io_sq_tail = (nvme->io_sq_tail + 1) % 64;
  // IO SQ1 Tail Doorbell is at 1000h + (2 * 1 * stride)
  volatile uint32_t *sq_db =
      (volatile uint32_t *)((uintptr_t)nvme->regs + 0x1000 +
                            (2 * nvme->db_stride));
  *sq_db = nvme->io_sq_tail;

  int timeout = 1000000;
  while (timeout--) {
    uint16_t status = cq[nvme->io_cq_head].status;
    if ((status & 0x1) == 1) { // Phase bit 1 = New entry
      if (res)
        memcpy(res, &cq[nvme->io_cq_head], sizeof(nvme_completion_t));

      nvme->io_cq_head = (nvme->io_cq_head + 1) % 256;
      // IO CQ1 Head Doorbell is at 1000h + (3 * stride)
      volatile uint32_t *cq_db =
          (volatile uint32_t *)((uintptr_t)nvme->regs + 0x1000 +
                                (3 * nvme->db_stride));
      *cq_db = nvme->io_cq_head;
      return 0;
    }
    for (int i = 0; i < 10; i++)
      __asm__ volatile("pause");
  }
  return -1;
}

static int nvme_io(struct nvme_controller *nvme, uint64_t lba, uint32_t count,
                   void *buf, int write) {
  size_t bytes = (size_t)count * 512;
  size_t pages = (bytes + 4095) / 4096;

  // Allocate physically contiguous buffer for DMA
  void *phys_buf = pmm_alloc_blocks(pages);
  void *virt_buf = (void *)((uintptr_t)phys_buf + pmm_get_hhdm_offset());

  if (write)
    memcpy(virt_buf, buf, bytes);

  nvme_cmd_t cmd = {0};
  cmd.cd0 = write ? 0x01 : 0x02; // Opcode: Write / Read
  cmd.nsid = 1;
  cmd.prp1 = (uint64_t)phys_buf;

  // Simple PRP handling: if it spans multiple pages, we need a PRP list
  // For now, if > 2 pages, we'll limit to 1 page or implement list.
  // Given pmm_alloc_blocks gives contiguous, we can just point prp2 to the
  // second page but ONLY if exactly 2 pages. For > 2, we need a list.
  if (pages == 2) {
    cmd.prp2 = (uint64_t)phys_buf + 4096;
  } else if (pages > 2) {
    // Create a PRP list
    uint64_t *prp_list =
        (uint64_t *)((uintptr_t)pmm_alloc() + pmm_get_hhdm_offset());
    for (size_t i = 0; i < pages - 1; i++) {
      prp_list[i] = (uint64_t)phys_buf + 4096 * (i + 1);
    }
    cmd.prp2 = (uint64_t)pmm_get_hhdm_offset()
                   ? (uint64_t)((uintptr_t)prp_list - pmm_get_hhdm_offset())
                   : (uint64_t)prp_list;
    // Optimization: since it's contiguous, it's easier.
  }

  cmd.cd10 = (uint32_t)lba;
  cmd.cd11 = (uint32_t)(lba >> 32);
  cmd.cd12 = (count - 1) & 0xFFFF; // NLB is 0-based

  if (nvme_submit_io_cmd(nvme, &cmd, NULL) != 0) {
    pmm_free_blocks(phys_buf, pages);
    return -1;
  }

  if (!write)
    memcpy(buf, virt_buf, bytes);

  pmm_free_blocks(phys_buf, pages);
  return 0;
}

static int nvme_block_read(struct block_device *dev, uint64_t lba,
                           uint32_t count, void *buf) {
  struct nvme_controller *nvme = (struct nvme_controller *)dev->driver_data;
  return nvme_io(nvme, lba, count, buf, 0);
}

static int nvme_block_write(struct block_device *dev, uint64_t lba,
                            uint32_t count, const void *buf) {
  struct nvme_controller *nvme = (struct nvme_controller *)dev->driver_data;
  return nvme_io(nvme, lba, count, (void *)buf, 1);
}

static struct device_id nvme_ids[] = {{.type = ID_PCI,
                                       .pci = {.match_class = true,
                                               .class = 0x01,
                                               .subclass = 0x08,
                                               .prog_if = 0x02}}};

static struct driver nvme_driver = {
    .name = "nvme", .ids = nvme_ids, .id_count = 1, .probe = nvme_probe};

void nvme_init(void) {
  nvme_count = 0;
  dm_register_driver(&nvme_driver);
}

void nvme_self_test(void) {
  klog_puts("[TEST] NVMe Phase 6: MSI-X Interrupts\n");
  if (nvme_count == 0) {
    klog_puts("       No NVMe controllers detected. FAIL.\n");
    return;
  }

  for (int i = 0; i < nvme_count; i++) {
    struct nvme_controller *nvme = &controllers[i];
    if (nvme_init_controller(nvme) == 0) {
      if (nvme_identify(nvme) == 0) {
        if (nvme_create_io_queues(nvme) == 0) {
          klog_puts("       Controller ");
          klog_uint64(i);
          klog_puts(": IO Queues established. SUCCESS.\n");

          // Final Test: Read LBA 0
          uint8_t buffer[512];
          if (nvme_block_read(&nvme->bdev, 0, 1, buffer) == 0) {
            klog_puts("       LBA 0 Read Test: SUCCESS. Signature: ");
            klog_hex32(
                *(uint32_t *)&buffer[510]); // Should be 0xAA55 if partitioned
            klog_puts("\n");
          } else {
            klog_puts("       LBA 0 Read Test: FAILED.\n");
          }
        } else {
          klog_puts("       Controller ");
          klog_uint64(i);
          klog_puts(": IO Queue setup FAILED.\n");
        }
      } else {
        klog_puts("       Controller ");
        klog_uint64(i);
        klog_puts(": Identification FAILED.\n");
      }
    } else {
      klog_puts("       Controller ");
      klog_uint64(i);
      klog_puts(": Initialization FAILED.\n");
    }
  }
  klog_puts("[TEST] NVMe Phase 6 complete.\n\n");
}

static int nvme_setup_msix(struct nvme_controller *nvme,
                           struct pci_device *pdev) {
  uint8_t cap_ptr = pci_find_capability(pdev, 0x11); // MSI-X
  if (!cap_ptr) {
    klog_puts(
        "[NVME]   MSI-X not supported by hardware. Falling back to polling.\n");
    return -1;
  }

  uint16_t msg_ctrl =
      pci_config_read16(pdev->bus, pdev->slot, pdev->func, cap_ptr + 2);
  uint32_t table_off_reg =
      pci_config_read32(pdev->bus, pdev->slot, pdev->func, cap_ptr + 4);
  uint8_t bir = table_off_reg & 0x7;
  uint32_t offset = table_off_reg & ~0x7;

  uint64_t phys_base = pdev->bar[bir] & 0xFFFFFFF0;
  if ((pdev->bar[bir] & 0x7) == 0x4) {
    phys_base |= ((uint64_t)pdev->bar[bir + 1] << 32);
  }

  uintptr_t table_virt = (phys_base + offset) + pmm_get_hhdm_offset();
  vmm_map_page(vmm_get_active_pml4(), table_virt, phys_base + offset,
               PAGE_FLAG_RW | PAGE_FLAG_PRESENT);
  nvme->msix_table_virt = (void *)table_virt;

  nvme->irq_vector = 0x2E; // Using a vector already in the IDT (46)
  register_interrupt_handler(nvme->irq_vector, nvme_irq_handler);

  msix_table_entry_t *entry = (msix_table_entry_t *)table_virt;
  entry->addr_lo = 0xFEE00000 | (lapic_get_id() << 12);
  entry->addr_hi = 0;
  entry->data = nvme->irq_vector;
  entry->vec_ctrl = 0;

  pci_config_write16(pdev->bus, pdev->slot, pdev->func, cap_ptr + 2,
                     msg_ctrl | 0x8000);

  klog_puts("[NVME]   MSI-X Enabled (Vector ");
  klog_hex32(nvme->irq_vector);
  klog_puts(")\n");

  return 0;
}

static void nvme_irq_handler(struct registers *regs) {
  (void)regs;
  klog_puts("[NVME] Interrupt received!\n");
}
