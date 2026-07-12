#include "acpi/acpi.h"
#include "apic/ioapic.h"
#include "apic/lapic.h"
#include "apic/lapic_timer.h"
#include "console/console.h"
#include "console/klog.h"
#include "cpu/fault.h"
#include "cpu/features.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/irq.h"
#include "cpu/isr.h"
#include "cpu/pic.h"
#include "cpu/tsc.h"
#include "drivers/audio/ac97.h"
#include "drivers/audio/audio_dsp.h"
#include "drivers/audio/hda.h"
#include "drivers/audio/sb16.h"
#include "drivers/gpu/drm/drm.h"
#include "drivers/input/evdev.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "drivers/manager/device.h"
#include "drivers/manager/dtb.h"
#include "drivers/net/rtl8139.h"
#include "drivers/pci/pci.h"
#include "drivers/serial.h"
#include "drivers/storage/ahci.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/block.h"
#include "drivers/storage/nvme.h"
#include "drivers/storage/ramdisk.h"
#include "drivers/timer/hpet.h"
#include "drivers/timer/pit.h"
#include "drivers/timer/rtc.h"

#include "drivers/usb/ehci.h"
#include "drivers/usb/ohci.h"
#include "drivers/usb/uhci.h"
#include "drivers/usb/usb.h"
#include "drivers/virtio/virtio.h"

#include "fb/framebuffer.h"
#include "fs/ext2.h"
#include "fs/ext4.h"
#include "fs/fat32.h"
#include "fs/procfs.h"
#include "fs/ramfs.h"
#include "fs/random.h"
#include "fs/vfs.h"
#include "io/io.h"
#include "mm/dma_alloc.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "mm/slab_cache.h"
#include "mm/tlb_shootdown.h"
#include "mm/vmm.h"
#include "net/core.h"
#include "net/dhcp.h"
#include "net/ipv4.h"
#include "net/ipv6.h"
#include "fs/sysfs.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "sched/sched.h"
#include "shell/shell.h"
#include "smp/cpu.h"
#include "socket/epoll.h"
#include "socket/socket.h"
#include "syscalls/syscall.h"
#include <limine.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((used,
               section(".limine_requests_start"))) static volatile uint64_t
    limine_requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[3] = LIMINE_BASE_REVISION(0);

__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_paging_mode_request
    paging_mode_request = {.id = LIMINE_PAGING_MODE_REQUEST_ID,
                           .revision = 0,
                           .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
                           .max_mode = 0,
                           .min_mode = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_rsdp_request
    rsdp_request = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_executable_address_request executable_address_request = {
        .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_dtb_request
    dtb_request = {.id = LIMINE_DTB_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests"))) static volatile struct
    limine_module_request module_request = {
        .id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t
    limine_requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

static void halt(void) {
  for (;;) {
    __asm__ volatile("hlt");
  }
}

static void init_thread_entry(void);
void kmain_high_half(void);

void restart_main_session(void) __attribute__((noreturn));
void restart_main_session(void) {
  // Restore kernel data segments since we are coming from a syscall
  __asm__ volatile("mov $0x10, %%ax\n"
                   "mov %%ax, %%ds\n"
                   "mov %%ax, %%es\n" ::
                       : "eax");
  __asm__ volatile("sti");

  struct thread *current = sched_get_current();
  uint64_t stack_top = current->stack_base + current->stack_size;
  stack_top &= ~0xFULL; // Maintain 16-byte alignment

  __asm__ volatile("mov %0, %%rsp\n"
                   "mov %0, %%rbp\n"
                   "jmp init_thread_entry\n" ::"r"(stack_top)
                   : "memory");
  while (1)
    ;
}

static void init_thread_entry(void) {
  net_core_start_worker();
  if (rtl8139_present()) {
    if (rtl8139_phase2_init() && rtl8139_phase3_init()) {
      net_phase4_init();
      net_phase5_init();
      net_phase6_init();
      ipv4_set_tcp_handler(tcp_input_ipv4);
      net_phase8_init();
      net_phase10_init();
      net_phase11_init();
      sysfs_populate_network();
    }
  }
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Init thread started\n");
  // Clear console only once when userland starts
  console_clear();
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                           " Console cleared, starting session...\n");

  while (1) {
    // Start AscentD as the main userspace session. Fall back to bash if the
    // root filesystem does not provide it yet.
    const char *sh_argv[] = {"/bin/ascentd", NULL};

    struct thread *current = sched_get_current();
    if (current) {
      current->is_main_session = true;
    }

    if (!process_exec_argv(sh_argv)) {
      // Fallback: try bash directly if sh failed
      const char *bash_argv[] = {"/bin/bash", NULL};
      if (!process_exec_argv(bash_argv)) {
        klog_puts("\n" KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                  " Failed to start Bash. Falling back to shell.\n");
        shell_init();
        shell_run();
        break;
      }
    }
  }
}

void kmain(void) {
  if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
    halt();
  }

  if (framebuffer_request.response == NULL ||
      framebuffer_request.response->framebuffer_count < 1) {
    halt();
  }

  struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
  serial_init();

  // Initialize basic PMM state (hhdm offset) so fb_init can work
  pmm_init_early(hhdm_request.response->offset);
  fb_init(fb);
  klog_set_screen_logging(true);

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                           " AscentOS Kernel Booting...\n");

  if (paging_mode_request.response != NULL) {
    if (paging_mode_request.response->mode == LIMINE_PAGING_MODE_X86_64_4LVL) {
      klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                               " Limine Paging Mode: 4-level (x86_64)\n");
    } else if (paging_mode_request.response->mode ==
               LIMINE_PAGING_MODE_X86_64_5LVL) {
      klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                               " Limine Paging Mode: 5-level (x86_64)\n");
    } else {
      klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                               " Limine Paging Mode: Unknown\n");
    }
  } else {
    klog_puts(KLOG_CLR_YELLOW
              "[ WARN ]" KLOG_CLR_RESET
              " Paging mode response not provided by Limine.\n");
  }

  if (memmap_request.response == NULL || hhdm_request.response == NULL) {
    klog_puts(KLOG_CLR_RED
              "[ FAIL ]" KLOG_CLR_RESET
              " Missing Limine memory map or HHDM responses. Halting.\n");
    halt();
  }

  pmm_init(memmap_request.response, hhdm_request.response->offset);

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                           " Physical Memory Manager (PMM) Initialized.\n");
  klog_puts("     Total RAM:  ");
  klog_uint64(pmm_get_total_memory() / (1024 * 1024));
  klog_puts(" MB\n");

  klog_puts("     Usable RAM: ");
  klog_uint64(pmm_get_usable_memory() / (1024 * 1024));
  klog_puts(" MB\n\n");

  gdt_init();
  cpu_features_init();
  tsc_init();
  idt_init();
  syscall_init();
  socket_init();
  epoll_init();
  isr_init_exceptions();

  pic_remap(32, 40);
  outb(0x21, 0xF8); // unmask IRQ0 (PIT), IRQ1 (Keyboard), and IRQ2 (Slave PIC)
  outb(0xA1, 0xEF); // unmask IRQ12 (Mouse)

  pit_init(100);
  rtc_init();
  klog_puts(KLOG_CLR_GREEN
            "[  OK  ]" KLOG_CLR_RESET
            " Legacy PIC Remapped, PIT 100Hz and RTC started.\n");

  keyboard_init();
  mouse_init();
  __asm__ volatile("sti"); 

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                           " Initializing Virtual Memory Manager (VMM)...\n");
  vmm_init();
  klog_puts("     Active CR3 Page Map hooked.\n");
  heap_init();
  slab_cache_init();

  extern kmem_cache_t *vma_cache;
  vma_cache = kmem_cache_create("vma", sizeof(struct vma), 8, NULL, NULL);

  dma_alloc_init();
  sb16_reserve_dma(); 
  console_init(fb);
  klog_set_screen_logging(false);
  dm_init();

  console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                              " Checking for platform DTB...\n");
  if (dtb_request.response && dtb_request.response->dtb_ptr) {
    dm_parse_dtb(dtb_request.response->dtb_ptr);
  } else {
    console_puts("      No platform DTB provided (standard for x86/ACPI).\n");
  }

  acpi_init(rsdp_request.response);

  hpet_init();

  acpi_parse_fadt();

  cpu_init();
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                           " Transitioning to kernel-allocated stack...\n");
  cpu_jump_to_stack(cpu_get_bsp()->stack_top, kmain_high_half);
}

void kmain_high_half(void) {
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                           " Switched to kernel-allocated stack.\n");

  uint32_t lapic_base = acpi_get_lapic_base();
  uint32_t ioapic_base = acpi_get_ioapic_base();

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                           " Initializing Scheduler...\n");
  sched_init();

  if (lapic_base && ioapic_base) {
    klog_puts("\n" KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
              " Switching to APIC interrupt mode...\n");

    __asm__ volatile("cli");

    pic_disable();
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                             " Legacy 8259 PIC disabled.\n");

    lapic_init((uint64_t)lapic_base);

    ioapic_init((uint64_t)ioapic_base, acpi_get_ioapic_gsi_base());

    irq_manager_sync();

    isr_set_apic_mode(true);

    __asm__ volatile("sti");

    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                             " APIC interrupt mode ACTIVE.\n\n");

    lapic_timer_init();

    if (hpet_is_backup_available()) {
      klog_puts(KLOG_CLR_BLUE "[ INFO ]" KLOG_CLR_RESET
                              " HPET available as backup timer.\n");
    }

    tlb_shootdown_init();

    cpu_init_aps();
  } else {
    klog_puts(KLOG_CLR_YELLOW
              "[ WARN ]" KLOG_CLR_RESET
              " APIC hardware not detected — staying with legacy PIC.\n\n");
  }

  uint64_t k_phys = 0;
  if (executable_address_request.response) {
    k_phys = executable_address_request.response->physical_base;
  }

  // Register an embedded disk.img before probing hardware-backed storage.
  // Its pages remain reserved by PMM for as long as ram0 uses them.
  ramdisk_init(module_request.response);

  pmm_reclaim_bootloader(k_phys);

  // Initialize Virtual Filesystem and Ramfs
  klog_puts(KLOG_CLR_GREEN
            "[  OK  ]" KLOG_CLR_RESET
            " Initializing RamFS & Virtual Filesystem (VFS)...\n");
  ramfs_init();

  extern kmem_cache_t *vfs_node_cache;
  vfs_node_cache =
      kmem_cache_create("vfs_node", sizeof(vfs_node_t), 8, NULL, NULL);

  shm_init();

  pci_init();
  usb_init();

  ehci_init();
  ehci_hand_to_companion(); 
  uhci_init();
  uhci_self_test();
  ohci_init();

  // VirtIO subsystem

  if (ahci_init() == 0) {
    ata_init();
  }

  nvme_init();

  // Mount root filesystem
  struct block_device *boot_dev = NULL;

  for (int i = 1; i < block_count(); i++) {
    boot_dev = block_get(i);
    if (boot_dev) {
      klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                               " Attempting to mount root from partition: ");
      klog_puts(boot_dev->name);
      klog_puts("...\n");
      if (ext4_mount_root(boot_dev) == 0) {
        goto mount_success;
      }
      if (ext2_mount_root(boot_dev) == 0) {
        goto mount_success;
      }
    }
  }

  // Fallback to raw disk
  boot_dev = block_get(0);
  if (boot_dev) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                             " Attempting to mount root from raw device: ");
    klog_puts(boot_dev->name);
    klog_puts("...\n");
    if (ext4_mount_root(boot_dev) == 0) {
      goto mount_success;
    }
    if (ext2_mount_root(boot_dev) == 0) {
      goto mount_success;
    }
  }

  klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                         " Failed to mount root filesystem on any device.\n");
  goto mount_fail;

mount_success:
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
                           " Root filesystem mounted successfully.\n");
  ramfs_mount_at("/dev");
  ramfs_mount_at("/tmp");
  ramfs_mount_at("/run");
  fault_init();

  extern void sysfs_init(void);
  sysfs_init();
  evdev_init();

  block_repopulate_devices();
  fb_register_vfs();
  drm_init();
  drm_register_vfs();
  fb_detect_drm_backend(); 
  mouse_register_vfs();
  random_register_vfs();
  procfs_init();

mount_fail:

  sb16_init();
  ac97_init();
  hda_init();

  sb16_register_vfs();
  ac97_register_vfs();
  hda_register_vfs();
  audio_dsp_register_vfs();

  net_core_init();
  rtl8139_phase1_init();

  struct thread *init_thread =
      sched_create_kernel_thread(init_thread_entry, cpu_get_bsp(), true);
  if (!init_thread) {
    klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                           " Failed to create init thread!\n");
    halt();
  }
  klog_puts("[KERNEL] init thread queued\n");

  for (;;) {
    __asm__ volatile("hlt");
  }
}
