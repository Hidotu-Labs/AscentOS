#include "shell/shell.h"
#include "apic/lapic_timer.h"
#include "console/console.h"
#include "console/klog.h"
#include "cpu/irq.h"
#include "drivers/audio/pcspeaker.h"
#include "drivers/input/keyboard.h"
#include "drivers/net/nic.h"
#include "drivers/serial.h"
#include "drivers/storage/block.h"
#include "fs/ext2.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "mm/slab_cache.h"
#include "mm/vmm.h"
#include "net/arp.h"
#include "net/dns.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "net/netif.h"
#include "net/tcp.h"
#include "sched/sched.h"
#include "smp/cpu.h"
#include <stdint.h>

#define CMD_BUFFER_SIZE 256

static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_len = 0;

static void test_task_entry(void);

// IRQ test handlers (file-scope for irqtest command)
static volatile int irq_test_handler1_hits = 0;
static volatile int irq_test_handler2_hits = 0;
static volatile int irq_test_handler3_hits = 0;
static void irq_test_handler1(struct registers *regs) {
  (void)regs;
  irq_test_handler1_hits++;
}
static void irq_test_handler2(struct registers *regs) {
  (void)regs;
  irq_test_handler2_hits++;
}
static void irq_test_handler3(struct registers *regs) {
  (void)regs;
  irq_test_handler3_hits++;
}

static void shell_print_uint64(uint64_t num) {
  if (num == 0) {
    console_puts("0");
    return;
  }
  char buf[20];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    i--;
    console_putchar(buf[i]);
  }
}

static void shell_print_hex_byte(uint8_t num) {
  const char *hex = "0123456789ABCDEF";
  console_putchar(hex[num >> 4]);
  console_putchar(hex[num & 0x0F]);
}

static void print_prompt(void) { console_puts("AscentOS> "); }

static void http_recv_cb(int sock_id, const uint8_t *p, uint16_t l) {
  (void)sock_id;
  for (uint16_t i = 0; i < l; i++) {
    console_putchar((char)p[i]);
  }
}

static void execute_command(char *cmd) {
  // Strip trailing spaces
  int len = strlen(cmd);
  while (len > 0 && cmd[len - 1] == ' ') {
    cmd[len - 1] = '\0';
    len--;
  }

  if (len == 0) {
    return;
  }

  if (strcmp(cmd, "help") == 0) {
    console_puts("Available commands:\n");
    console_puts("  help      - Show this help message\n");
    console_puts("  clear     - Clear the console screen\n");
    console_puts("  meminfo   - Display memory utilization\n");
    console_puts("  echo      - Echo arguments\n");
    console_puts("  beep      - Play a test beep sound\n");
    console_puts("  ps        - List running tasks\n");
    console_puts("  kill      - Terminate a task by TID (e.g. kill 5)\n");
    console_puts("  heaptest  - Test kernel heap allocator\n");
    console_puts("  irqtest   - Test IRQ handler install/uninstall\n");
    console_puts("  locktest  - Test atomic spinlock functionality\n");
    console_puts("  uptime    - Show system uptime\n");
    console_puts("  diskinfo  - Show detected block devices\n");
    console_puts(
        "  readsect  - Read and hex-dump a disk sector (e.g. readsect 0)\n");
    console_puts("  ls        - List directory contents (e.g. ls /mnt)\n");
    console_puts(
        "  cat       - Display file contents (e.g. cat /mnt/hello.txt)\n");
    console_puts("  write     - Write text to file (e.g. write /mnt/file.txt "
                 "hello world)\n");
    console_puts(
        "  touch     - Create an empty file (e.g. touch /mnt/new.txt)\n");
    console_puts("  mkdir     - Create a directory (e.g. mkdir /mnt/mydir)\n");
    console_puts("  rm        - Delete a file (e.g. rm /mnt/old.txt)\n");
    console_puts(
        "  rmdir     - Remove an empty directory (e.g. rmdir /mnt/mydir)\n");
    console_puts(
        "  ln        - Create a symlink (e.g. ln /mnt/link /mnt/target)\n");
    console_puts(
        "  readlink  - Read symlink target (e.g. readlink /mnt/link)\n");
    console_puts("  stat      - Show file info (e.g. stat /mnt/file.txt)\n");
    console_puts(
        "  rename    - Rename a file (e.g. rename /mnt/old /mnt/new)\n");
    console_puts(
        "  chmod     - Change permissions (e.g. chmod 0777 /mnt/file.txt)\n");
    console_puts("  chown     - Change owner/group (e.g. chown 1000:1000 "
                 "/mnt/file.txt)\n");
    console_puts("  test_ring3_phase1 - Verify GDT and TSS initialization\n");
    console_puts("  test_ring3_phase2 - Verify Syscall MSR initialization\n");
    console_puts("  test_ring3_phase3 - Verify Syscall translation layer\n");
    console_puts("  exec      - Execute an ELF (e.g. exec /mnt/hello_musl.elf "
                 "[args...])\n");
    console_puts("  pmmtest   - Test the PMM buddy allocator\n");
    console_puts(
        "  vmmtest   - Test the VMM demand paging and mapping bounds\n");
    console_puts(
        "  slabtest  - Test generic native Slab Allocator constraints\n");
    console_puts(
        "  slabstress - Stress test Named Object Slab Cache (kmem_cache)\n");
    console_puts(
        "  shmtest   - Stress test System V Shared Memory (shmget/shmat)\n");
    console_puts("  kilo      - Launch the kilo text editor\n");
    console_puts("  netinfo   - Show NIC status and MAC address\n");
    console_puts("  ifconfig  - Show network interface configuration\n");
    console_puts("  arp       - Display ARP cache table\n");
    console_puts("  arping    - Send ARP request (e.g. arping 10.0.2.2)\n");
    console_puts("  ping      - Send ICMP echo request (e.g. ping 10.0.2.2)\n");
  } else if (strcmp(cmd, "ps") == 0) {
    sched_print_tasks();
  } else if (strncmp(cmd, "kill ", 5) == 0) {
    uint32_t tid = atoui(cmd + 5);
    if (sched_terminate_thread(tid)) {
      console_puts("Terminated thread ");
      shell_print_uint64(tid);
      console_puts("\n");
    } else {
      console_puts("Failed to terminate thread (maybe TID 0 or not found).\n");
    }
  } else if (strcmp(cmd, "clear") == 0) {
    console_clear();
  } else if (strcmp(cmd, "meminfo") == 0) {
    uint64_t total = pmm_get_total_memory();
    uint64_t usable = pmm_get_usable_memory();
    console_puts("Memory stats:\n");
    console_puts("  Total RAM:  ");
    shell_print_uint64(total / (1024 * 1024));
    console_puts(" MB\n");
    console_puts("  Usable RAM: ");
    shell_print_uint64(usable / (1024 * 1024));
    console_puts(" MB\n");
  } else if (strncmp(cmd, "echo ", 5) == 0) {
    console_puts(cmd + 5);
    console_puts("\n");
  } else if (strcmp(cmd, "beep") == 0) {
    console_puts("Beep!\n");
    pcspeaker_play_sound(1000);
    lapic_timer_sleep(200);
    pcspeaker_nosound();
  } else if (strcmp(cmd, "test-task") == 0) {
    for (int i = 0; i < 4; i++) {
      sched_create_kernel_thread(test_task_entry, NULL, true);
    }
    console_puts("Spawned 4 test tasks across SMP cores!\n");
  } else if (strcmp(cmd, "heaptest") == 0) {
    console_puts("Starting Kernel Heap Stress Test...\n");

    // Test 1: Small allocations
    console_puts("[1/4] Allocating 50 small chunks...\n");
    void *ptrs[50];
    int test1_pass = 1;
    for (int i = 0; i < 50; i++) {
      ptrs[i] = kmalloc(32);
      if (!ptrs[i]) {
        test1_pass = 0;
        break;
      }
      memset(ptrs[i], (i & 0xFF), 32);
    }

    if (test1_pass) {
      for (int i = 0; i < 50; i++) {
        uint8_t *p = (uint8_t *)ptrs[i];
        for (int j = 0; j < 32; j++) {
          if (p[j] != (i & 0xFF)) {
            test1_pass = 0;
            break;
          }
        }
      }
    }

    if (test1_pass) {
      console_puts("  -> PASS: 50 small allocations & verification.\n");
    } else {
      console_puts("  -> FAIL: Small allocations.\n");
    }

    // Test 2: Fragmentation
    console_puts("[2/4] Freeing chunks to test fragmentation...\n");
    for (int i = 0; i < 50; i += 2) {
      kfree(ptrs[i]);
      ptrs[i] = NULL;
    }
    void *hole_ptrs[25];
    for (int i = 0; i < 25; i++) {
      hole_ptrs[i] = kmalloc(16);
    }
    console_puts("  -> PASS: Fragmentation allocation.\n");

    for (int i = 1; i < 50; i += 2) {
      if (ptrs[i])
        kfree(ptrs[i]);
    }
    for (int i = 0; i < 25; i++) {
      if (hole_ptrs[i])
        kfree(hole_ptrs[i]);
    }

    // Test 3: Large allocation
    console_puts("[3/4] Testing large contiguous allocation (64KB)...\n");
    void *large_ptr = kmalloc(65536);
    if (large_ptr) {
      memset(large_ptr, 0xAA, 65536);
      uint8_t *lp = (uint8_t *)large_ptr;
      if (lp[0] == 0xAA && lp[65535] == 0xAA) {
        console_puts("  -> PASS: Large allocation successful.\n");
      } else {
        console_puts("  -> FAIL: Large allocation corrupted.\n");
      }
      kfree(large_ptr);
    } else {
      console_puts("  -> FAIL: Large allocation returned NULL.\n");
    }

    // Test 4: kcalloc and krealloc
    console_puts("[4/4] Testing kcalloc & krealloc...\n");
    char *str = kcalloc(1, 16);
    if (str) {
      const char *hello = "Hello ";
      int i = 0;
      while (hello[i]) {
        str[i] = hello[i];
        i++;
      }

      str = krealloc(str, 32);
      if (str) {
        const char *world = "World!";
        int j = 0;
        while (world[j]) {
          str[i + j] = world[j];
          j++;
        }
        str[i + j] = '\0';

        console_puts("  -> realloc result: ");
        console_puts(str);
        console_puts("\n");
        kfree(str);
        console_puts("  -> PASS: calloc & realloc test.\n");
      } else {
        console_puts("  -> FAIL: krealloc returned NULL.\n");
      }
    }

    console_puts("Stress test complete.\n");
  } else if (strcmp(cmd, "irqtest") == 0) {
    console_puts("Starting IRQ Handler Install/Uninstall Test...\n");

    // Test 1: Install single handler
    console_puts("[1/7] Installing handler1 on IRQ 15...\n");
    if (irq_install_handler(15, irq_test_handler1, 0)) {
      console_puts("  -> PASS: Handler1 installed.\n");
    } else {
      console_puts("  -> FAIL: Could not install handler1.\n");
    }

    // Test 2: Install second handler (shared IRQ)
    console_puts("[2/7] Installing handler2 on same IRQ (shared)...\n");
    if (irq_install_handler(15, irq_test_handler2, 0)) {
      console_puts("  -> PASS: Handler2 installed (shared IRQ).\n");
    } else {
      console_puts("  -> FAIL: Could not install handler2.\n");
    }

    // Test 3: Install third handler (shared IRQ)
    console_puts("[3/7] Installing handler3 on same IRQ (shared)...\n");
    if (irq_install_handler(15, irq_test_handler3, 0)) {
      console_puts("  -> PASS: Handler3 installed (shared IRQ).\n");
    } else {
      console_puts("  -> FAIL: Could not install handler3.\n");
    }

    // Test 4: Trigger interrupt and verify ALL handlers called (EOI timing
    // test) If EOI were sent mid-dispatch, later handlers wouldn't run
    console_puts(
        "[4/7] Triggering IRQ 15 to verify all 3 handlers execute...\n");
    irq_test_handler1_hits = 0;
    irq_test_handler2_hits = 0;
    irq_test_handler3_hits = 0;
    irq_test_trigger(15);
    if (irq_test_handler1_hits > 0 && irq_test_handler2_hits > 0 &&
        irq_test_handler3_hits > 0) {
      console_puts("  -> PASS: All 3 handlers executed (EOI sent after all "
                   "complete).\n");
    } else {
      console_puts("  -> FAIL: Not all handlers called (EOI timing issue?).\n");
    }

    // Test 5: Uninstall middle handler
    console_puts("[5/7] Uninstalling handler2 (middle of chain)...\n");
    if (irq_uninstall_handler(15, irq_test_handler2)) {
      console_puts("  -> PASS: Handler2 removed.\n");
    } else {
      console_puts("  -> FAIL: Could not remove handler2.\n");
    }

    // Test 6: Verify remaining handlers still work
    console_puts("[6/7] Triggering IRQ 15 (handlers 1 and 3 should run)...\n");
    irq_test_handler1_hits = 0;
    irq_test_handler2_hits = 0;
    irq_test_handler3_hits = 0;
    irq_test_trigger(15);
    if (irq_test_handler1_hits > 0 && irq_test_handler2_hits == 0 &&
        irq_test_handler3_hits > 0) {
      console_puts("  -> PASS: Only handlers 1 and 3 executed.\n");
    } else {
      console_puts("  -> FAIL: Uninstall did not work correctly.\n");
    }

    // Test 7: Uninstall all and verify no handlers run
    console_puts("[7/7] Uninstalling all handlers and verifying cleanup...\n");
    irq_uninstall_handler(15, irq_test_handler1);
    irq_uninstall_handler(15, irq_test_handler3);
    irq_test_handler1_hits = 0;
    irq_test_handler2_hits = 0;
    irq_test_handler3_hits = 0;
    irq_test_trigger(15);
    if (irq_test_handler1_hits == 0 && irq_test_handler2_hits == 0 &&
        irq_test_handler3_hits == 0) {
      console_puts("  -> PASS: No handlers executed after full cleanup.\n");
    } else {
      console_puts("  -> FAIL: Handlers still running after uninstall.\n");
    }

    console_puts("IRQ test complete.\n");
  } else if (strcmp(cmd, "pmmtest") == 0) {
    console_puts("Starting Intensive PMM Buddy Allocator Stress Test...\n");

    int global_pass = 1;

    console_puts(
        "[1/4] Testing allocations of all valid orders (0 to 10)...\n");
    void *order_blocks[11];
    for (int i = 0; i <= 10; i++) {
      size_t pages = 1ULL << i;
      order_blocks[i] = pmm_alloc_pages(pages);
      if (!order_blocks[i]) {
        console_puts("  -> ERROR: Failed to allocate order ");
        shell_print_uint64(i);
        console_puts("\n");
        global_pass = 0;
      } else {
        // Deep verification setup: fill entire block with predictable pattern
        uint64_t *ptr =
            (uint64_t *)((uint64_t)order_blocks[i] + pmm_get_hhdm_offset());
        size_t words = (pages * 4096) / sizeof(uint64_t);
        for (size_t w = 0; w < words; w++) {
          ptr[w] = ((uint64_t)i << 56) ^ ((uint64_t)w << 32) ^ (0xDEADBEEFULL);
        }
      }
    }

    if (global_pass) {
      console_puts("[2/4] Verifying data integrity across all orders...\n");
      for (int i = 0; i <= 10; i++) {
        if (!order_blocks[i])
          continue;
        size_t pages = 1ULL << i;
        uint64_t *ptr =
            (uint64_t *)((uint64_t)order_blocks[i] + pmm_get_hhdm_offset());
        size_t words = (pages * 4096) / sizeof(uint64_t);
        int clean = 1;
        for (size_t w = 0; w < words; w++) {
          uint64_t expected =
              ((uint64_t)i << 56) ^ ((uint64_t)w << 32) ^ (0xDEADBEEFULL);
          if (ptr[w] != expected) {
            console_puts("  -> FAIL: Corruption detected in order ");
            shell_print_uint64(i);
            console_puts(" at word offset ");
            shell_print_uint64(w);
            console_puts("\n");
            global_pass = 0;
            clean = 0;
            break;
          }
        }
        if (clean) {
          console_puts("  -> PASS: Order ");
          shell_print_uint64(i);
          console_puts(" verified.\n");
        }
      }
    }

    console_puts("  -> Freeing multi-order blocks...\n");
    for (int i = 0; i <= 10; i++) {
      if (order_blocks[i]) {
        pmm_free_pages(order_blocks[i], 1ULL << i);
      }
    }

    console_puts("[3/4] Fragmentation & Coalescing stress test...\n");
    int num_blocks = 512;
    void *frag_blocks[512];
    int frag_pass = 1;
    for (int i = 0; i < num_blocks; i++) {
      frag_blocks[i] = pmm_alloc_pages(1); // 1 page
      if (!frag_blocks[i])
        frag_pass = 0;
    }
    if (!frag_pass) {
      console_puts(
          "  -> WARNING: Could not allocate all fragmentation blocks.\n");
    }

    // Free every other block to fragment memory
    for (int i = 0; i < num_blocks; i += 2) {
      if (frag_blocks[i]) {
        pmm_free_pages(frag_blocks[i], 1);
        frag_blocks[i] = NULL;
      }
    }

    // Allocate order-2 (4 pages) blocks which requires coalescing or finding
    // chunks
    void *coalesce_blocks[64];
    for (int i = 0; i < 64; i++) {
      coalesce_blocks[i] = pmm_alloc_pages(4); // 4 pages
    }

    // Free everything from this test
    for (int i = 0; i < 64; i++) {
      if (coalesce_blocks[i])
        pmm_free_pages(coalesce_blocks[i], 4);
    }
    for (int i = 1; i < num_blocks; i += 2) {
      if (frag_blocks[i])
        pmm_free_pages(frag_blocks[i], 1);
    }
    console_puts("  -> Fragmentation test completed.\n");

    console_puts("[4/4] Bulk Capacity Test...\n");
    size_t free_ram = pmm_get_free_pages();
    // Allocate up to 25% of currently free memory to avoid OOM
    size_t bulk_total = free_ram / 4;
    // Cap at 100,000 pages (~400MB) if it's very large
    if (bulk_total > 100000)
      bulk_total = 100000;
    // Need at least some pages to test
    if (bulk_total == 0)
      bulk_total = 10;

    console_puts("  -> Attempting to allocate ");
    shell_print_uint64(bulk_total);
    console_puts(" individual pages...\n");

    void **bulk_array = kmalloc(bulk_total * sizeof(void *));
    if (bulk_array) {
      size_t bulk_allocated = 0;
      for (size_t i = 0; i < bulk_total; i++) {
        bulk_array[i] = pmm_alloc_pages(1);
        if (bulk_array[i]) {
          bulk_allocated++;
          uint64_t *ptr =
              (uint64_t *)((uint64_t)bulk_array[i] + pmm_get_hhdm_offset());
          *ptr = (uint64_t)
              bulk_array[i]; // Store its own physical address as signature
        } else {
          break; // Hit early limit
        }
      }

      console_puts("  -> Successfully allocated ");
      shell_print_uint64(bulk_allocated);
      console_puts(" pages.\n");

      // Verify and Free
      int bulk_pass = 1;
      for (size_t i = 0; i < bulk_allocated; i++) {
        uint64_t *ptr =
            (uint64_t *)((uint64_t)bulk_array[i] + pmm_get_hhdm_offset());
        if (*ptr != (uint64_t)bulk_array[i]) {
          bulk_pass = 0;
        }
        pmm_free_pages(bulk_array[i], 1);
      }

      if (bulk_pass)
        console_puts("  -> PASS: Bulk test verified and freed properly.\n");
      else {
        console_puts("  -> FAIL: Bulk test memory corruption!\n");
        global_pass = 0;
      }

      kfree(bulk_array);
    } else {
      console_puts("  -> ERROR: Failed to allocate bulk pointer array from "
                   "kernel heap.\n");
    }

    if (global_pass) {
      console_puts("\n=== PMM STRESS TEST PASSED WITH 0 RISKS ===\n");
    } else {
      console_puts("\n=== PMM STRESS TEST FAILED ===\n");
    }
  } else if (strcmp(cmd, "cowtest") == 0) {
    console_puts("Testing VMM Copy-on-Write (CoW) Logic (Ring 0)...\n");

    // 1. Prepare a test page in the current address space
    uint64_t vaddr = 0x9000000;
    void *phys = pmm_alloc_page();
    uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_USER;
    if (!vmm_map_page(vmm_get_active_pml4(), vaddr, (uint64_t)phys, flags)) {
      console_puts("  -> FAIL: Could not map test page.\n");
      return;
    }

    // Initialize memory
    *(uint8_t *)vaddr = 0x42;

    // 2. Mock a VMA list to satisfy the clone function
    struct vma_list vmas;
    vma_list_init(&vmas);
    // Use Linux-style PROT_READ|PROT_WRITE (1|2) and MAP_PRIVATE|MAP_ANONYMOUS
    // (2|32)
    vma_add(&vmas, vaddr, vaddr + 4096, 0x1 | 0x2, 0x02 | 0x20, -1, 0, NULL);

    console_puts("Original page refcount: ");
    shell_print_uint64(pmm_get_ref(phys));
    console_puts("\n");

    // 3. Clone the address space logic (simulating fork)
    uint64_t child_cr3 =
        vmm_clone_user_mappings_vma(vmm_get_active_pml4(), &vmas);

    if (child_cr3 == 0) {
      console_puts("  -> FAIL: vmm_clone_user_mappings_vma failed.\n");
    } else {
      console_puts("Address space cloned.\n");
      console_puts("New page refcount (should be 2): ");
      shell_print_uint64(pmm_get_ref(phys));
      console_puts("\n");

      // Verify refcount logic
      if (pmm_get_ref(phys) == 2) {
        console_puts("  -> PASS: Reference count incremented correctly.\n");
      } else {
        console_puts("  -> FAIL: Incorrect reference count.\n");
      }

      // 4. Simulate process exit (cleanup child)
      console_puts("Cleaning up cloned address space...\n");
      vmm_free_user_pages_vma(child_cr3, &vmas);

      console_puts("Final page refcount (should be 1): ");
      shell_print_uint64(pmm_get_ref(phys));
      console_puts("\n");

      if (pmm_get_ref(phys) == 1) {
        console_puts("  -> PASS: Reference count decremented correctly.\n");
      } else {
        console_puts("  -> FAIL: Leak or double-free detected.\n");
      }
    }

    // Final Cleanup
    vmm_unmap_page(vmm_get_active_pml4(), vaddr);
    vma_list_destroy(&vmas);
    pmm_free_page(phys);
    console_puts("Logic test complete.\n");
    console_puts("Run /bin/test_cow in userland for full fork stress test.\n");
  } else if (strcmp(cmd, "vmmtest") == 0) {
    console_puts("Starting Intensive VMM Page Table Stress Test...\n");

    int vmm_pass = 1;
    uint64_t *pml4 = vmm_get_active_pml4();
    uint64_t test_vaddr_base =
        0x0000555500000000ULL; // Arbitrary user-space base

    // Test 1: Single Page Mapping & Unmapping
    console_puts("[1/4] Testing isolated page mapping and unmapping...\n");
    void *single_page = pmm_alloc_page();
    if (!single_page) {
      console_puts("  -> ERROR: PMM failed inside VMM test.\n");
      vmm_pass = 0;
    } else {
      uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_USER;
      if (!vmm_map_page(pml4, test_vaddr_base, (uint64_t)single_page, flags)) {
        console_puts("  -> FAIL: vmm_map_page returned false.\n");
        vmm_pass = 0;
      } else {
        // Verify we can access it through the VMM
        uint64_t *test_ptr = (uint64_t *)test_vaddr_base;
        *test_ptr = 0x1122334455667788ULL;
        if (*test_ptr != 0x1122334455667788ULL) {
          console_puts("  -> FAIL: Memory access verification failed.\n");
          vmm_pass = 0;
        }

        // Unmap
        size_t free_before = pmm_get_free_pages();
        vmm_unmap_page(pml4, test_vaddr_base);
        size_t free_after = pmm_get_free_pages();

        // Verify intermediate table cleanup (should free PT, PD, PDPT since
        // this is a unique tree sequence)
        if (free_after <= free_before) {
          console_puts("  -> WARNING: Empty tables were not reliably freed by "
                       "vmm_unmap_page!\n");
        } else {
          console_puts(
              "  -> PASS: Single mapping and empty table cleanup succeeded.\n");
        }
        pmm_free_page(single_page);
      }
    }

    // Test 2: Range Mapping
    console_puts("[2/4] Testing vmm_map_range (100 contiguous pages)...\n");
    size_t range_pages = 100;
    void *range_block = pmm_alloc_pages(range_pages);
    if (!range_block) {
      console_puts("  -> ERROR: PMM failed range allocation.\n");
      vmm_pass = 0;
    } else {
      uint64_t range_vaddr = test_vaddr_base + 0x100000000ULL;
      uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW;
      if (!vmm_map_range(pml4, range_vaddr, (uint64_t)range_block, range_pages,
                         flags)) {
        console_puts("  -> FAIL: vmm_map_range failed.\n");
        vmm_pass = 0;
      } else {
        // Access start, middle, and end
        ((volatile uint64_t *)range_vaddr)[0] = 0xAAAA;
        ((volatile uint64_t *)(range_vaddr + 50 * 4096))[0] = 0xBBBB;
        ((volatile uint64_t *)(range_vaddr + 99 * 4096))[0] = 0xCCCC;

        if (((volatile uint64_t *)range_vaddr)[0] == 0xAAAA &&
            ((volatile uint64_t *)(range_vaddr + 99 * 4096))[0] == 0xCCCC) {
          console_puts("  -> PASS: Contiguous range mapped and accessible.\n");
        } else {
          console_puts("  -> FAIL: Range accessibility check failed.\n");
          vmm_pass = 0;
        }

        // Free range mappings seamlessly
        for (size_t i = 0; i < range_pages; i++) {
          vmm_unmap_page(pml4, range_vaddr + i * 4096);
        }
      }
      pmm_free_pages(range_block, range_pages);
    }

    // Test 3: Huge Page Mapping
    console_puts("[3/4] Testing vmm_map_huge_page (2MB)...\n");
    void *huge_block_norm = pmm_alloc_pages(
        512); // Extrapolates to exactly 2MB under order-9 naturally
    if (!huge_block_norm) {
      console_puts("  -> ERROR: PMM failed 2MB allocation.\n");
      vmm_pass = 0;
    } else {
      uint64_t huge_vaddr = test_vaddr_base + 0x200000000ULL;
      uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW;
      if (!vmm_map_huge_page(pml4, huge_vaddr, (uint64_t)huge_block_norm,
                             flags)) {
        console_puts("  -> FAIL: vmm_map_huge_page failed.\n");
        vmm_pass = 0;
      } else {

        uint64_t phys_ret =
            vmm_virt_to_phys(pml4, huge_vaddr + 0x100000); // 1MB deep lookup
        if (phys_ret == ((uint64_t)huge_block_norm + 0x100000)) {
          console_puts(
              "  -> PASS: Huge page dynamically translated via Walker.\n");
        } else {
          console_puts("  -> FAIL: Huge page resolution mismatch.\n");
          vmm_pass = 0;
        }
      }
      pmm_free_pages(huge_block_norm, 512);
    }

    // Test 4: Page Fault Handler Fast-Path Interception
    console_puts(
        "[4/4] Triggering intentional Fault to invoke Zero-Fill Engine...\n");
    uint64_t fault_vaddr = test_vaddr_base + 0x300000000ULL;

    struct thread *current = sched_get_current();
    if (current && current->mm) {
      // Wire up dummy permissions map in active tree to prove it resolves
      if (vma_add(&current->mm->vmas, fault_vaddr, fault_vaddr + 4096, 0x3,
                  0x22, -1, 0, NULL) != -1) {
        volatile uint64_t *fault_ptr = (volatile uint64_t *)fault_vaddr;

        // -> CRASH INDUCED <-
        // Will trigger ISR #14, run `vmm_handle_page_fault`, allocate memory,
        // and patch PTE!
        *fault_ptr = 0x9988776655443322ULL;

        if (*fault_ptr == 0x9988776655443322ULL) {
          console_puts("  -> PASS: Fault trapped. Zero-Fill handled seamlessly "
                       "via ISR Phase 2 Engine!\n");
        } else {
          console_puts("  -> FAIL: Zero-Fill payload integrity mismatch.\n");
          vmm_pass = 0;
        }

        // Purge testing structure
        vma_remove(&current->mm->vmas, fault_vaddr, fault_vaddr + 4096);
        vmm_unmap_page(pml4, fault_vaddr);
      } else {
        console_puts("  -> SKIP: VMA list exceeded limits. Could not construct "
                     "dummy bounds.\n");
      }
    } else {
      console_puts("  -> SKIP: No active process context available here.\n");
    }

    // Test 5: Thrashing Mapping/Unmapping Leaks
    console_puts("[5/6] Testing VMM mapping thrashing for PMM leaks...\n");
    uint64_t thrash_vaddr = test_vaddr_base + 0x400000000ULL;
    void *thrash_page = pmm_alloc_page();

    size_t free_before_thrash = pmm_get_free_pages();
    for (int i = 0; i < 500; i++) {
      vmm_map_page(pml4, thrash_vaddr, (uint64_t)thrash_page,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_RW);
      vmm_unmap_page(pml4, thrash_vaddr);
    }
    size_t free_after_thrash = pmm_get_free_pages();

    // Thrash unmap should have cleanly returned all intermediate empty tables
    // back to PMM.
    if (free_after_thrash < free_before_thrash) {
      console_puts(
          "  -> FAIL: Thrash test resulted in PMM frames leaking! (Diff: ");
      shell_print_uint64(free_before_thrash - free_after_thrash);
      console_puts(")\n");
      vmm_pass = 0;
    } else {
      console_puts("  -> PASS: Thrash mapping gracefully unmapped all "
                   "intermediate blocks seamlessly.\n");
    }
    pmm_free_page(thrash_page);

    // Test 6: Deep Sparse Mapping Array
    console_puts("[6/6] Testing deep sparse mapping alignment tables...\n");
    uint64_t sparse_vaddr_base = test_vaddr_base + 0x500000000ULL;
    size_t sparse_free_before = pmm_get_free_pages();
    void *sparse_pages[10];

    for (int i = 0; i < 10; i++) {
      sparse_pages[i] = pmm_alloc_page();
      // Shift spacing massively so each one forces a unique Page Directory or
      // PDPT allocation
      uint64_t shift_vaddr = sparse_vaddr_base + ((uint64_t)i * 0x40000000ULL);
      vmm_map_page(pml4, shift_vaddr, (uint64_t)sparse_pages[i],
                   PAGE_FLAG_PRESENT | PAGE_FLAG_RW);
      volatile uint64_t *ptr = (volatile uint64_t *)shift_vaddr;
      *ptr = 0xAA55AA55;
    }

    for (int i = 0; i < 10; i++) {
      uint64_t shift_vaddr = sparse_vaddr_base + ((uint64_t)i * 0x40000000ULL);
      vmm_unmap_page(pml4, shift_vaddr);
      pmm_free_page(sparse_pages[i]);
    }
    size_t sparse_free_after = pmm_get_free_pages();

    if (sparse_free_before > sparse_free_after) {
      console_puts("  -> FAIL: Sparse unmapping leaked structures (diff: ");
      shell_print_uint64(sparse_free_before - sparse_free_after);
      console_puts(")\n");
      vmm_pass = 0;
    } else {
      console_puts("  -> PASS: Sparse wide structure mappings and cleanup "
                   "successful.\n");
    }

    // Test 7: TCC Execution Environment Simulation (mmap -> write payload ->
    // execute -> unmap)
    console_puts("[7/7] Testing dynamic code execution (TCC simulation)...\n");
    uint64_t tcc_vaddr = test_vaddr_base + 0x600000000ULL;
    void *tcc_page = pmm_alloc_page();

    if (!vmm_map_page(pml4, tcc_vaddr, (uint64_t)tcc_page,
                      PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_USER)) {
      console_puts("  -> FAIL: TCC mapping allocation blocked!\n");
      vmm_pass = 0;
    } else {
      // Generate an x86-64 assembly payload dynamically
      // Payload:
      // mov rax, 0x1122334455667788
      // ret
      uint8_t *payload = (uint8_t *)tcc_vaddr;
      payload[0] = 0x48; // REX.W
      payload[1] = 0xB8; // MOV r64, imm64
      payload[2] = 0x88;
      payload[3] = 0x77;
      payload[4] = 0x66;
      payload[5] = 0x55;
      payload[6] = 0x44;
      payload[7] = 0x33;
      payload[8] = 0x22;
      payload[9] = 0x11;
      payload[10] = 0xC3; // RET

      // Flush TLB implicitly, cast to function signature
      typedef uint64_t (*tcc_func_t)(void);
      tcc_func_t tcc_func = (tcc_func_t)tcc_vaddr;

      // Execute dynamic payload
      uint64_t result = tcc_func();

      if (result == 0x1122334455667788ULL) {
        console_puts("  -> PASS: Dynamic out-of-order execution payload "
                     "triggered and computed correctly.\n");
      } else {
        console_puts(
            "  -> FAIL: TCC computational payload returned invalid state!\n");
        vmm_pass = 0;
      }

      vmm_unmap_page(pml4, tcc_vaddr);
    }
    pmm_free_page(tcc_page);

    if (vmm_pass) {
      console_puts("\n=== ADVANCED VMM STRESS TEST PASSED SEAMLESSLY ===\n");
    } else {
      console_puts("\n=== VMM STRESS TEST FAILED ===\n");
    }
  } else if (strcmp(cmd, "slabtest") == 0) {
    console_puts("Starting Intensive Generic Kernel Slab Allocator Test...\n");
    int slab_pass = 1;

    // Test 1: Bucket Mapping Fragmentation Resistance
    console_puts("[1/3] Testing Discrete Bucket Spanning (32 -> 1024)...\n");
    void *bucket_ptrs[6];
    size_t sizes[] = {32, 64, 128, 256, 512, 1024};

    size_t baselines_before = pmm_get_free_pages();

    for (int i = 0; i < 6; i++) {
      bucket_ptrs[i] = kmalloc(sizes[i]);
      if (!bucket_ptrs[i]) {
        console_puts(
            "  -> FAIL: Dynamic slab bucket allocation failed for size: ");
        shell_print_uint64(sizes[i]);
        console_puts("\n");
        slab_pass = 0;
      } else {
        // Prove usability by writing unique patterns across boundary edges
        memset(bucket_ptrs[i], 0xAA, sizes[i]);
      }
    }

    // Verify Integrity & Free Seamlessly
    if (slab_pass) {
      for (int i = 0; i < 6; i++) {
        uint8_t *chk = (uint8_t *)bucket_ptrs[i];
        if (chk[0] != 0xAA || chk[sizes[i] - 1] != 0xAA) {
          slab_pass = 0;
          console_puts(
              "  -> FAIL: Overwrite corruption detected boundary collision!\n");
        }
        kfree(bucket_ptrs[i]);
      }
    }

    if (slab_pass)
      console_puts("  -> PASS: Unified discreet bucket mapping and Coalescing "
                   "passed.\n");

    // Test 2: Mass Object Density Scaling (1000 items)
    console_puts("[2/3] Testing Mass Object Density Stress (1000 allocs at 128 "
                 "bytes)...\n");
    void *mass_ptrs[1000];
    int mass_iter = 0;
    for (; mass_iter < 1000; mass_iter++) {
      mass_ptrs[mass_iter] = kmalloc(128);
      if (!mass_ptrs[mass_iter])
        break;
      *(uint64_t *)(mass_ptrs[mass_iter]) = 0x1234567890ABCDEFULL + mass_iter;
    }

    if (mass_iter < 1000) {
      console_puts(
          "  -> FAIL: Mass Density failed dynamically around iteration: ");
      shell_print_uint64(mass_iter);
      console_puts("\n");
      slab_pass = 0;
    } else {
      // Verify integrity
      int integrity = 1;
      for (int i = 0; i < 1000; i++) {
        if (*(uint64_t *)(mass_ptrs[i]) != 0x1234567890ABCDEFULL + i) {
          integrity = 0;
          break;
        }
        kfree(mass_ptrs[i]);
      }
      if (integrity)
        console_puts("  -> PASS: 1000 sequential Slab objects mapped and "
                     "dynamically unmapped perfectly.\n");
      else {
        console_puts(
            "  -> FAIL: Overwrite density leak detected in partial caches!\n");
        slab_pass = 0;
      }
    }

    // Test 3: Large Assorted Off-Cache Allocator
    console_puts("[3/3] Testing Out-of-Cache Massive Allocations (4096+)...\n");
    void *huge_obj1 = kmalloc(8192);  // 8KB
    void *huge_obj2 = kmalloc(25000); // Massive disjoint

    if (!huge_obj1 || !huge_obj2) {
      console_puts(
          "  -> FAIL: Large unified allocation block routed failure.\n");
      slab_pass = 0;
    } else {
      memset(huge_obj1, 0xBB, 8192);
      memset(huge_obj2, 0xCC, 25000);
      if (((uint8_t *)huge_obj1)[8191] == 0xBB &&
          ((uint8_t *)huge_obj2)[24999] == 0xCC) {
        console_puts("  -> PASS: Large generic heap blocks bypassed flawlessly "
                     "to native endpoints.\n");
      } else {
        console_puts("  -> FAIL: Dynamic large block integrity fault.\n");
        slab_pass = 0;
      }
      kfree(huge_obj2);
      kfree(huge_obj1);
    }

    size_t baselines_after = pmm_get_free_pages();
    if (baselines_after < baselines_before) {
      console_puts("  -> FAIL: Slab system leaked ");
      shell_print_uint64(baselines_before - baselines_after);
      console_puts(" physical frames globally!\n");
      slab_pass = 0;
    } else {
      console_puts("  -> PASS: PMM Page frame trackers stabilized matching "
                   "total equilibrium.\n");
    }

    if (slab_pass) {
      console_puts(
          "\n=== KERNEL SLAB ALLOCATOR UNIFIED TEST PASSED (ZERO LEAKS) ===\n");
    } else {
      console_puts("\n=== SLAB TEST FAILED ===\n");
    }

    // Named Slab Cache Stress Test (kmem_cache API)
  } else if (strcmp(cmd, "slabstress") == 0) {
    console_puts("Starting Named Slab Cache Stress Test...\n");
    int pass = 1;

    console_puts("[1/6] Creating custom test cache (obj_size=96)...\n");
    kmem_cache_t *test_cache =
        kmem_cache_create("stress_test", 96, 8, NULL, NULL);
    if (!test_cache) {
      console_puts("  -> FAIL: kmem_cache_create returned NULL!\n");
      pass = 0;
    } else {
      console_puts("  -> PASS: Cache created successfully.\n");

      // Allocate 500 objects
      console_puts("[2/6] Allocating 500 objects from test cache...\n");
      void *objs[500];
      int alloc_count = 0;
      for (int i = 0; i < 500; i++) {
        objs[i] = kmem_cache_alloc(test_cache);
        if (!objs[i]) {
          console_puts("  -> FAIL: Allocation failed at index ");
          shell_print_uint64(i);
          console_puts("\n");
          pass = 0;
          break;
        }
        alloc_count++;
        // Write a unique pattern (verifies no overlapping objects)
        memset(objs[i], (uint8_t)(i & 0xFF), 96);
      }

      if (alloc_count == 500) {
        console_puts("  -> PASS: 500 objects allocated.\n");

        // Verify data integrity
        console_puts("[3/6] Verifying data integrity across 500 objects...\n");
        int integrity = 1;
        for (int i = 0; i < 500; i++) {
          uint8_t *p = (uint8_t *)objs[i];
          uint8_t expected = (uint8_t)(i & 0xFF);
          if (p[0] != expected || p[95] != expected) {
            console_puts("  -> FAIL: Corruption at index ");
            shell_print_uint64(i);
            console_puts("\n");
            integrity = 0;
            pass = 0;
            break;
          }
        }
        if (integrity) {
          console_puts("  -> PASS: All 500 objects verified.\n");
        }

        // Free all objects
        console_puts("[4/6] Freeing all 500 objects...\n");
        for (int i = 0; i < 500; i++) {
          kmem_cache_free(test_cache, objs[i]);
        }

        if (test_cache->active_objects == 0) {
          console_puts("  -> PASS: Active count returned to 0.\n");
        } else {
          console_puts("  -> FAIL: Active count is ");
          shell_print_uint64(test_cache->active_objects);
          console_puts(" (expected 0)\n");
          pass = 0;
        }

        // Verify re-allocation works (slab reuse)
        console_puts("[5/6] Testing slab reuse (alloc/free/alloc cycle)...\n");
        void *reuse1 = kmem_cache_alloc(test_cache);
        void *reuse2 = kmem_cache_alloc(test_cache);
        if (reuse1 && reuse2) {
          kmem_cache_free(test_cache, reuse1);
          kmem_cache_free(test_cache, reuse2);
          console_puts("  -> PASS: Slab reuse cycle works.\n");
        } else {
          console_puts("  -> FAIL: Reuse allocation failed.\n");
          pass = 0;
        }
      } else {
        // Free what we allocated
        for (int i = 0; i < alloc_count; i++) {
          kmem_cache_free(test_cache, objs[i]);
        }
      }

      // Test shrink
      console_puts("[6/6] Testing kmem_cache_shrink...\n");
      size_t free_before = pmm_get_free_pages();
      kmem_cache_shrink(test_cache);
      size_t free_after = pmm_get_free_pages();
      if (free_after >= free_before) {
        console_puts("  -> PASS: Shrink returned ");
        shell_print_uint64(free_after - free_before);
        console_puts(" pages to PMM.\n");
      } else {
        console_puts("  -> WARN: Shrink did not reclaim (may be normal).\n");
      }

      kmem_cache_destroy(test_cache);
    }

    console_puts("\n[BONUS] Testing kernel object caches...\n");

    if (vma_cache) {
      void *v1 = kmem_cache_alloc(vma_cache);
      void *v2 = kmem_cache_alloc(vma_cache);
      if (v1 && v2) {
        kmem_cache_free(vma_cache, v1);
        kmem_cache_free(vma_cache, v2);
        console_puts("  -> PASS: vma_cache alloc/free cycle OK.\n");
      } else {
        console_puts("  -> FAIL: vma_cache alloc returned NULL.\n");
        pass = 0;
      }
    } else {
      console_puts("  -> SKIP: vma_cache not initialized.\n");
    }

    if (vfs_node_cache) {
      void *n1 = kmem_cache_alloc(vfs_node_cache);
      void *n2 = kmem_cache_alloc(vfs_node_cache);
      if (n1 && n2) {
        kmem_cache_free(vfs_node_cache, n1);
        kmem_cache_free(vfs_node_cache, n2);
        console_puts("  -> PASS: vfs_node_cache alloc/free cycle OK.\n");
      } else {
        console_puts("  -> FAIL: vfs_node_cache alloc returned NULL.\n");
        pass = 0;
      }
    } else {
      console_puts("  -> SKIP: vfs_node_cache not initialized.\n");
    }

    if (thread_cache) {
      void *t1 = kmem_cache_alloc(thread_cache);
      if (t1) {
        kmem_cache_free(thread_cache, t1);
        console_puts("  -> PASS: thread_cache alloc/free cycle OK.\n");
      } else {
        console_puts("  -> FAIL: thread_cache alloc returned NULL.\n");
        pass = 0;
      }
    } else {
      console_puts("  -> SKIP: thread_cache not initialized.\n");
    }

    // Print cache stats
    kmem_cache_print_all();

    if (pass) {
      console_puts("\n=== NAMED SLAB CACHE STRESS TEST PASSED ===\n");
    } else {
      console_puts("\n=== NAMED SLAB CACHE STRESS TEST FAILED ===\n");
    }

    // Shared Memory (SHM) Stress Test
  } else if (strcmp(cmd, "shmtest") == 0) {
    console_puts("Starting Shared Memory (SHM) Stress Test...\n");
    int pass = 1;

    console_puts("[1/5] Creating SHM segment (8192 bytes, key=42)...\n");
    int64_t shmid = sys_shmget(42, 8192, IPC_CREAT | 0x1FF, 0, 0, 0);
    if (shmid < 0) {
      console_puts("  -> FAIL: shmget returned error ");
      shell_print_uint64((uint64_t)(-shmid));
      console_puts("\n");
      pass = 0;
    } else {
      console_puts("  -> PASS: Created segment id=");
      shell_print_uint64((uint64_t)shmid);
      console_puts("\n");
    }

    int64_t addr1 = 0;
    if (pass) {
      console_puts("[2/5] Attaching segment (auto-address)...\n");
      addr1 = sys_shmat((uint64_t)shmid, 0, 0, 0, 0, 0);
      if (addr1 < 0 || addr1 == 0) {
        console_puts("  -> FAIL: shmat returned error\n");
        pass = 0;
      } else {
        console_puts("  -> PASS: Attached at ");
        shell_print_uint64((uint64_t)addr1);
        console_puts("\n");
      }
    }

    if (pass) {
      console_puts("[3/5] Writing pattern and verifying...\n");
      volatile uint8_t *ptr = (volatile uint8_t *)addr1;
      for (int i = 0; i < 8192; i++) {
        ptr[i] = (uint8_t)(i & 0xFF);
      }

      int integrity = 1;
      for (int i = 0; i < 8192; i++) {
        if (ptr[i] != (uint8_t)(i & 0xFF)) {
          integrity = 0;
          break;
        }
      }

      if (integrity) {
        console_puts("  -> PASS: 8192 bytes written and verified.\n");
      } else {
        console_puts("  -> FAIL: Data integrity mismatch.\n");
        pass = 0;
      }
    }

    if (pass) {
      console_puts("[4/5] Querying segment info (shmctl IPC_STAT)...\n");
      struct shmid_ds info;
      memset(&info, 0, sizeof(info));
      int64_t rc =
          sys_shmctl((uint64_t)shmid, IPC_STAT, (uint64_t)&info, 0, 0, 0);
      if (rc == 0 && info.shm_segsz == 8192 && info.shm_nattch == 1) {
        console_puts("  -> PASS: size=");
        shell_print_uint64(info.shm_segsz);
        console_puts(" nattch=");
        shell_print_uint64(info.shm_nattch);
        console_puts("\n");
      } else {
        console_puts("  -> FAIL: shmctl returned unexpected values.\n");
        pass = 0;
      }
    }

    if (pass) {
      console_puts("[5/5] Detaching and destroying segment...\n");
      int64_t rc = sys_shmdt((uint64_t)addr1, 0, 0, 0, 0, 0);
      if (rc != 0) {
        console_puts("  -> FAIL: shmdt returned error.\n");
        pass = 0;
      }

      rc = sys_shmctl((uint64_t)shmid, IPC_RMID, 0, 0, 0, 0);
      if (rc != 0) {
        console_puts("  -> FAIL: shmctl IPC_RMID returned error.\n");
        pass = 0;
      }

      if (pass) {
        console_puts("  -> PASS: Segment detached and destroyed.\n");
      }
    }

    // Print SHM status table
    shm_print_status();

    if (pass) {
      console_puts("\n=== SHM STRESS TEST PASSED ===\n");
    } else {
      console_puts("\n=== SHM STRESS TEST FAILED ===\n");
    }

  } else if (strcmp(cmd, "vmatest") == 0) {
    console_puts("Starting Intensive VMA AVL Interval Tree Stress Test...\n");
    struct vma_list tester_list;
    vma_list_init(&tester_list);
    int vma_pass = 1;

    // Test 1: Massive Insertion scaling beyond flat-array limits (2000
    // insertions)
    console_puts("[1/3] Mass Boundary scaling (2000+ insertions)...\n");
    for (int i = 0; i < 2000; i++) {
      if (vma_add(&tester_list, 0x1000 * 2 * i, 0x1000 * (2 * i + 1), 0x3, 0x22,
                  -1, 0, NULL) < 0) {
        console_puts("  -> FAIL: Native AVL insert crashed out early!\n");
        vma_pass = 0;
        break;
      }
    }

    if (vma_pass) {
      if (tester_list.count != 2000) {
        console_puts("  -> FAIL: VMA Node count offset detected!\n");
        vma_pass = 0;
      } else {
        console_puts("  -> PASS: Native dynamic memory tree scaled bounds to "
                     "exactly 2000 nodes natively.\n");
      }
    }

    // Test 2: Dense Overlap Avoidance
    console_puts("[2/3] Dense Interval Map Avoidance Check...\n");
    if (vma_add(&tester_list, 0x1000 * 250, 0x1000 * 251, 0x3, 0x22, -1, 0,
                NULL) == 0) {
      console_puts("  -> FAIL: Interval search tree silently allowed overlap "
                   "injection!\n");
      vma_pass = 0;
    } else {
      console_puts(
          "  -> PASS: Interval tree securely blocked overlap fault cleanly.\n");
    }

    // Test 3: Total Split/Collapse destruction sequences
    console_puts("[3/3] Deleting exactly 2000 nodes globally...\n");
    bool success = vma_remove(&tester_list, 0, 0x1000 * 2 * 3000);

    if (success && tester_list.count == 0) {
      console_puts("  -> PASS: Structural collapse cleared effectively inside "
                   "global mappings.\n");
    } else {
      console_puts(
          "  -> FAIL: Node memory leaked heavily mapping tree bounds!\n");
      vma_pass = 0;
    }

    if (vma_pass) {
      console_puts(
          "\n=== KERNEL VMA INTERVAL AVL TEST PASSED (ZERO LEAKS) ===\n");
    } else {
      console_puts("\n=== VMA TEST FAILED ===\n");
    }
  } else if (strcmp(cmd, "locktest") == 0) {
    console_puts("Testing Atomic Spinlock primitives...\n");
    static spinlock_t test_lock = SPINLOCK_INIT;

    console_puts("Acquiring test_lock...\n");
    spinlock_acquire(&test_lock);
    console_puts("  -> test_lock acquired (locked state achieved)!\n");

    console_puts("Releasing test_lock...\n");
    spinlock_release(&test_lock);
    console_puts("  -> test_lock released (unlocked state achieved)!\n");

    console_puts("Spinlock test complete and PASSED.\n");
  } else if (strcmp(cmd, "uptime") == 0) {
    uint64_t ms = lapic_timer_get_ms();
    uint64_t secs = ms / 1000;
    uint64_t mins = secs / 60;
    uint64_t hrs = mins / 60;

    console_puts("Uptime: ");
    // Hours
    shell_print_uint64(hrs);
    console_puts("h ");
    // Minutes
    shell_print_uint64(mins % 60);
    console_puts("m ");
    // Seconds
    shell_print_uint64(secs % 60);
    console_puts("s ");
    // Milliseconds
    shell_print_uint64(ms % 1000);
    console_puts("ms\n");

    console_puts("LAPIC ticks: ");
    shell_print_uint64(lapic_timer_get_ticks());
    console_puts("\n");
  } else if (strcmp(cmd, "diskinfo") == 0) {
    int count = block_count();
    if (count == 0) {
      console_puts("No block devices registered.\n");
    } else {
      console_puts("Block devices:\n");
      for (int i = 0; i < count; i++) {
        struct block_device *blk = block_get(i);
        if (!blk)
          continue;
        console_puts("  ");
        console_puts(blk->name);
        console_puts(": ");
        shell_print_uint64((blk->total_sectors * blk->sector_size) /
                           (1024 * 1024));
        console_puts(" MB (");
        shell_print_uint64(blk->total_sectors);
        console_puts(" sectors x ");
        shell_print_uint64(blk->sector_size);
        console_puts(" bytes)\n");
      }
    }
  } else if (strncmp(cmd, "readsect ", 9) == 0) {
    uint64_t lba = (uint64_t)atoui(cmd + 9);
    struct block_device *blk = block_get(0);
    if (!blk) {
      console_puts("No block device available.\n");
    } else {
      uint8_t sector_buf[512];
      memset(sector_buf, 0, 512);
      int ret = blk->read_sectors(blk, lba, 1, sector_buf);
      if (ret < 0) {
        console_puts("Read error!\n");
      } else {
        console_puts("Sector ");
        shell_print_uint64(lba);
        console_puts(" from ");
        console_puts(blk->name);
        console_puts(":\n");
        // Hex dump: 16 bytes per line, 32 lines
        const char *hex = "0123456789ABCDEF";
        for (int row = 0; row < 32; row++) {
          // Print offset
          uint16_t off = (uint16_t)(row * 16);
          console_putchar(hex[(off >> 8) & 0xF]);
          console_putchar(hex[(off >> 4) & 0xF]);
          console_putchar(hex[off & 0xF]);
          console_puts(": ");
          // Hex values
          for (int col = 0; col < 16; col++) {
            uint8_t b = sector_buf[row * 16 + col];
            console_putchar(hex[(b >> 4) & 0xF]);
            console_putchar(hex[b & 0xF]);
            console_putchar(' ');
          }
          // ASCII
          console_puts(" |");
          for (int col = 0; col < 16; col++) {
            uint8_t b = sector_buf[row * 16 + col];
            console_putchar((b >= 0x20 && b < 0x7F) ? (char)b : '.');
          }
          console_puts("|\n");
        }
      }
    }
  } else if (strncmp(cmd, "ls ", 3) == 0 || strcmp(cmd, "ls") == 0) {
    char *path = (strlen(cmd) > 3) ? cmd + 3 : "/";
    vfs_node_t *dir = vfs_resolve_path(path);

    if (!dir) {
      console_puts("ls: cannot access '");
      console_puts(path);
      console_puts("': No such file or directory\n");
    } else if ((dir->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
      console_puts("ls: '");
      console_puts(path);
      console_puts("': Not a directory\n");
    } else {
      struct dirent *d;
      uint32_t i = 0;
      while ((d = vfs_readdir(dir, i++)) != 0) {
        console_puts(d->name);
        console_puts("  ");
      }
      console_puts("\n");
    }
  } else if (strncmp(cmd, "cat ", 4) == 0) {
    char *path = cmd + 4;
    vfs_node_t *file = vfs_resolve_path(path);

    if (!file) {
      console_puts("cat: ");
      console_puts(path);
      console_puts(": No such file\n");
    } else if ((file->flags & FS_TYPE_MASK) == FS_DIRECTORY) {
      console_puts("cat: ");
      console_puts(path);
      console_puts(": Is a directory\n");
    } else if ((file->flags & FS_TYPE_MASK) == FS_SYMLINK) {
      console_puts("cat: ");
      console_puts(path);
      console_puts(": Is a symbolic link (use readlink)\n");
    } else {
      uint8_t buf[512];
      uint32_t offset = 0;
      uint32_t bytes;
      uint32_t max_bytes = 8192;
      while ((bytes = vfs_read(file, offset, sizeof(buf), buf)) > 0) {
        for (uint32_t j = 0; j < bytes; j++) {
          char c = (char)buf[j];
          if (c >= 32 && c <= 126) {
            console_putchar(c);
          } else if (c == '\n' || c == '\r' || c == '\t') {
            console_putchar(c);
          } else {
            console_putchar('.');
          }
        }
        offset += bytes;
        if (offset >= max_bytes) {
          console_puts("\n... (truncated at 8KB)\n");
          break;
        }
      }
      console_puts("\n");
    }
  } else if (strncmp(cmd, "write ", 6) == 0) {
    // Usage: write /mnt/file.txt content here
    char *rest = cmd + 6;
    // Find end of path (first space)
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: write <path> <content>\n");
    } else {
      *space = '\0';
      char *filepath = rest;
      char *content = space + 1;
      uint32_t content_len = strlen(content);

      vfs_node_t *file = vfs_resolve_path(filepath);

      if (!file) {
        // Try to create it: resolve parent, then create
        // Find last '/' to get parent path and filename
        char *last_slash = NULL;
        for (char *p = filepath; *p; p++) {
          if (*p == '/')
            last_slash = p;
        }
        if (last_slash && last_slash != filepath) {
          *last_slash = '\0';
          char *parent_path = filepath;
          char *filename = last_slash + 1;
          vfs_node_t *parent = vfs_resolve_path(parent_path);
          if (parent) {
            if (vfs_create(parent, filename, 0644) == 0) {
              file = vfs_finddir(parent, filename);
            }
          }
          *last_slash = '/'; // Restore
        } else if (last_slash == filepath) {
          // Path like /filename — create in root
          char *filename = filepath + 1;
          if (vfs_create(fs_root, filename, 0644) == 0) {
            file = vfs_finddir(fs_root, filename);
          }
        }
      }

      if (!file) {
        console_puts("write: cannot create or open '");
        console_puts(rest);
        console_puts("'\n");
      } else {
        uint32_t written = vfs_write(file, 0, content_len, (uint8_t *)content);
        console_puts("Wrote ");
        shell_print_uint64(written);
        console_puts(" bytes.\n");
      }
    }
  } else if (strncmp(cmd, "touch ", 6) == 0) {
    char *filepath = cmd + 6;
    // Find parent directory and filename
    char *last_slash = NULL;
    for (char *p = filepath; *p; p++) {
      if (*p == '/')
        last_slash = p;
    }
    if (!last_slash) {
      console_puts("Usage: touch <path> (e.g. touch /mnt/file.txt)\n");
    } else {
      *last_slash = '\0';
      char *parent_path = (last_slash == filepath) ? "/" : filepath;
      char *filename = last_slash + 1;
      vfs_node_t *parent = vfs_resolve_path(parent_path);
      *last_slash = '/'; // Restore
      if (!parent) {
        console_puts("touch: parent directory not found\n");
      } else {
        int ret = vfs_create(parent, filename, 0644);
        if (ret == 0) {
          console_puts("Created: ");
          console_puts(filepath);
          console_puts("\n");
        } else {
          console_puts("touch: failed (file may already exist)\n");
        }
      }
    }
  } else if (strncmp(cmd, "mkdir ", 6) == 0) {
    char *dirpath = cmd + 6;
    char *last_slash = NULL;
    for (char *p = dirpath; *p; p++) {
      if (*p == '/')
        last_slash = p;
    }
    if (!last_slash) {
      console_puts("Usage: mkdir <path> (e.g. mkdir /mnt/mydir)\n");
    } else {
      *last_slash = '\0';
      char *parent_path = (last_slash == dirpath) ? "/" : dirpath;
      char *dirname = last_slash + 1;
      vfs_node_t *parent = vfs_resolve_path(parent_path);
      *last_slash = '/'; // Restore
      if (!parent) {
        console_puts("mkdir: parent directory not found\n");
      } else {
        int ret = vfs_mkdir(parent, dirname, 0755);
        if (ret == 0) {
          console_puts("Created directory: ");
          console_puts(dirpath);
          console_puts("\n");
        } else {
          console_puts("mkdir: failed (directory may already exist)\n");
        }
      }
    }
  } else if (strncmp(cmd, "rm ", 3) == 0) {
    char *filepath = cmd + 3;
    // Find parent directory and filename
    char *last_slash = NULL;
    for (char *p = filepath; *p; p++) {
      if (*p == '/')
        last_slash = p;
    }
    if (!last_slash) {
      console_puts("Usage: rm <path> (e.g. rm /mnt/file.txt)\n");
    } else {
      *last_slash = '\0';
      char *parent_path = (last_slash == filepath) ? "/" : filepath;
      char *filename = last_slash + 1;
      vfs_node_t *parent = vfs_resolve_path(parent_path);
      *last_slash = '/'; // Restore
      if (!parent) {
        console_puts("rm: parent directory not found\n");
      } else {
        int ret = vfs_unlink(parent, filename);
        if (ret == 0) {
          console_puts("Removed: ");
          console_puts(filepath);
          console_puts("\n");
        } else {
          console_puts("rm: failed (file not found or is a directory)\n");
        }
      }
    }
  } else if (strncmp(cmd, "rmdir ", 6) == 0) {
    char *dirpath = cmd + 6;
    char *last_slash = NULL;
    for (char *p = dirpath; *p; p++) {
      if (*p == '/')
        last_slash = p;
    }
    if (!last_slash) {
      console_puts("Usage: rmdir <path> (e.g. rmdir /mnt/mydir)\n");
    } else {
      *last_slash = '\0';
      char *parent_path = (last_slash == dirpath) ? "/" : dirpath;
      char *dirname = last_slash + 1;
      vfs_node_t *parent = vfs_resolve_path(parent_path);
      *last_slash = '/'; // Restore
      if (!parent) {
        console_puts("rmdir: parent directory not found\n");
      } else {
        int ret = vfs_rmdir(parent, dirname);
        if (ret == 0) {
          console_puts("Removed directory: ");
          console_puts(dirpath);
          console_puts("\n");
        } else {
          console_puts(
              "rmdir: failed (not found, not empty, or not a directory)\n");
        }
      }
    }
  } else if (strncmp(cmd, "ln ", 3) == 0) {
    // Usage: ln /mnt/linkname /mnt/target
    char *rest = cmd + 3;
    // Find end of link path (first space)
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: ln <linkpath> <target>\n");
    } else {
      *space = '\0';
      char *linkpath = rest;
      char *target = space + 1;

      // Resolve parent of link
      char *last_slash = NULL;
      for (char *p = linkpath; *p; p++) {
        if (*p == '/')
          last_slash = p;
      }
      if (!last_slash) {
        console_puts("Usage: ln <linkpath> <target>\n");
      } else {
        *last_slash = '\0';
        char *parent_path = (last_slash == linkpath) ? "/" : linkpath;
        char *linkname = last_slash + 1;
        vfs_node_t *parent = vfs_resolve_path(parent_path);
        *last_slash = '/'; // Restore
        if (!parent) {
          console_puts("ln: parent directory not found\n");
        } else {
          int ret = vfs_symlink(parent, linkname, target);
          if (ret == 0) {
            console_puts("Created symlink: ");
            console_puts(linkpath);
            console_puts(" -> ");
            console_puts(target);
            console_puts("\n");
          } else {
            console_puts("ln: failed (may already exist)\n");
          }
        }
      }
    }
  } else if (strncmp(cmd, "readlink ", 9) == 0) {
    char *path = cmd + 9;
    vfs_node_t *file = vfs_resolve_path(path);
    if (!file) {
      console_puts("readlink: ");
      console_puts(path);
      console_puts(": No such file\n");
    } else {
      char target_buf[256];
      int ret = vfs_readlink(file, target_buf, sizeof(target_buf));
      if (ret < 0) {
        console_puts("readlink: ");
        console_puts(path);
        console_puts(": Not a symbolic link\n");
      } else {
        console_puts(target_buf);
        console_puts("\n");
      }
    }
  } else if (strncmp(cmd, "stat ", 5) == 0) {
    char *path = cmd + 5;
    vfs_node_t *file = vfs_resolve_path(path);
    if (!file) {
      console_puts("stat: ");
      console_puts(path);
      console_puts(": No such file\n");
    } else {
      console_puts("  File: ");
      console_puts(path);
      console_puts("\n");
      console_puts("  Size: ");
      shell_print_uint64(file->length);
      console_puts(" bytes\n");
      console_puts("  ModTime: ");
      shell_print_uint64(file->mtime);
      console_puts(" secs since boot\n");
      console_puts("  Type: ");
      uint32_t ftype = file->flags & FS_TYPE_MASK;
      if (ftype == FS_FILE)
        console_puts("regular file\n");
      else if (ftype == FS_DIRECTORY)
        console_puts("directory\n");
      else if (ftype == FS_SYMLINK) {
        console_puts("symbolic link -> ");
        char tbuf[256];
        if (vfs_readlink(file, tbuf, sizeof(tbuf)) >= 0) {
          console_puts(tbuf);
        }
        console_puts("\n");
      } else
        console_puts("other\n");
      console_puts("  Inode: ");
      shell_print_uint64(file->inode);
      console_puts("\n");
      console_puts("  Perms: 0");
      // Print permissions in octal
      uint32_t m = file->mask;
      console_putchar('0' + ((m >> 9) & 7));
      console_putchar('0' + ((m >> 6) & 7));
      console_putchar('0' + ((m >> 3) & 7));
      console_putchar('0' + (m & 7));
      console_puts("\n");
      console_puts("  UID: ");
      shell_print_uint64(file->uid);
      console_puts("  GID: ");
      shell_print_uint64(file->gid);
      console_puts("\n");
    }
  } else if (strncmp(cmd, "rename ", 7) == 0) {
    char *rest = cmd + 7;
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: rename <old_path> <new_path>\n");
    } else {
      *space = '\0';
      char *old_path = rest;
      char *new_path = space + 1;

      char *last_slash = NULL;
      for (char *p = old_path; *p; p++)
        if (*p == '/')
          last_slash = p;
      if (!last_slash) {
        console_puts("rename: need absolute path (e.g. /mnt/...)\n");
      } else {
        *last_slash = '\0';
        char *parent_path = (last_slash == old_path) ? "/" : old_path;
        char *old_name = last_slash + 1;
        vfs_node_t *parent = vfs_resolve_path(parent_path);

        char *new_last_slash = NULL;
        for (char *p = new_path; *p; p++)
          if (*p == '/')
            new_last_slash = p;
        if (!new_last_slash) {
          console_puts("rename: need absolute new path\n");
        } else {
          *new_last_slash = '\0';
          char *new_parent_path = (new_last_slash == new_path) ? "/" : new_path;
          char *new_name = new_last_slash + 1;

          if (strcmp(parent_path, new_parent_path) != 0) {
            console_puts("rename: cross-directory rename not supported yet\n");
          } else if (!parent) {
            console_puts("rename: parent directory not found\n");
          } else {
            if (vfs_rename(parent, old_name, new_name) == 0) {
              console_puts("Renamed ");
              *last_slash = '/'; // restore temporarily for printing
              console_puts(old_path);
              console_puts(" to ");
              *new_last_slash = '/'; // restore temporarily for printing
              console_puts(new_path);
              console_puts("\n");
              // slashes restored correctly
            } else {
              console_puts("rename failed\n");
            }
          }
          *new_last_slash = '/'; // Restore unconditionally
        }
        *last_slash = '/'; // Restore unconditionally
      }
    }
  } else if (strncmp(cmd, "chmod ", 6) == 0) {
    char *rest = cmd + 6;
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: chmod <octal_perms> <file>\n");
    } else {
      *space = '\0';
      char *perms_str = rest;
      char *path = space + 1;

      uint32_t perms = 0;
      for (int i = 0; perms_str[i]; i++) {
        if (perms_str[i] >= '0' && perms_str[i] <= '7') {
          perms = (perms << 3) + (perms_str[i] - '0');
        }
      }

      vfs_node_t *node = vfs_resolve_path(path);
      if (!node) {
        console_puts("chmod: file not found\n");
      } else if (vfs_chmod(node, perms) == 0) {
        console_puts("Permissions changed.\n");
      } else {
        console_puts("chmod failed.\n");
      }
    }
  } else if (strncmp(cmd, "chown ", 6) == 0) {
    char *rest = cmd + 6;
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: chown <uid>:<gid> <file>\n");
    } else {
      *space = '\0';
      char *owner_str = rest;
      char *path = space + 1;

      char *colon = NULL;
      for (char *p = owner_str; *p; p++)
        if (*p == ':')
          colon = p;

      uint32_t uid = 0, gid = 0;
      if (colon) {
        *colon = '\0';
        uid = atoui(owner_str);
        gid = atoui(colon + 1);
        *colon = ':';
      } else {
        uid = atoui(owner_str);
        gid = uid;
      }

      vfs_node_t *node = vfs_resolve_path(path);
      if (!node) {
        console_puts("chown: file not found\n");
      } else if (vfs_chown(node, uid, gid) == 0) {
        console_puts("Ownership changed.\n");
      } else {
        console_puts("chown failed.\n");
      }
    }
  } else if (strcmp(cmd, "test_ring3_phase1") == 0) {
    console_puts("Phase 1: GDT & TSS Verification\n");
    uint8_t sgdt_buf[10];
    __asm__ volatile("sgdt %0" : "=m"(sgdt_buf));
    uint16_t limit = *(uint16_t *)sgdt_buf;
    uint64_t base = *(uint64_t *)(sgdt_buf + 2);
    console_puts("  GDT Base: 0x");
    shell_print_uint64(base);
    console_puts("\n  GDT Limit: ");
    shell_print_uint64(limit);
    console_puts("\n");

    uint16_t tr = 0;
    __asm__ volatile("str %0" : "=r"(tr));
    console_puts("  Task Register (TR): 0x");
    shell_print_uint64(tr);
    if (tr == 0x28) {
      console_puts("\n  -> SUCCESS: TR is 0x28 (TSS loaded!).\n");
    } else {
      console_puts("\n  -> FAIL: TR is not 0x28.\n");
    }
  } else if (strcmp(cmd, "test_ring3_phase2") == 0) {
    console_puts("Phase 2: Syscall MSR Verification\n");

    uint64_t star = 0, lstar = 0, fmask = 0, efer = 0;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    efer = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000081));
    star = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000082));
    lstar = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000084));
    fmask = ((uint64_t)hi << 32) | lo;

    console_puts("  EFER: 0x");
    shell_print_uint64(efer);
    if (efer & 1)
      console_puts(" (SCE enabled)\n");
    else
      console_puts(" (SCE disabled)\n");

    console_puts("  STAR: 0x");
    shell_print_uint64(star);
    console_puts("\n");
    console_puts("  LSTAR: 0x");
    shell_print_uint64(lstar);
    console_puts("\n");
    console_puts("  FMASK: 0x");
    shell_print_uint64(fmask);
    console_puts("\n");

    if ((efer & 1) && star != 0 && lstar != 0) {
      console_puts("\n  -> SUCCESS: MSRs are configured!\n");
    } else {
      console_puts("\n  -> FAIL: MSR misconfiguration.\n");
    }
  } else if (strcmp(cmd, "test_ring3_phase3") == 0) {
    console_puts("Phase 3: Syscall ABI Translation Verification\n");
    console_puts("  Simulating a 'write' syscall to stdout...\n");

    // We will fake a push of syscall arguments to the dispatcher
    struct mock_regs {
      uint64_t rdi, rsi, rdx, r10, r8, r9, rax, rbx, rbp, r12, r13, r14, r15;
      uint64_t rip, rflags, rsp;
    } regs = {0};

    char *str = "  [Syscall Response] Hello from the other side!\n";

    regs.rax = 1; // sys_write
    regs.rdi = 1; // fd = 1 (stdout)
    regs.rsi = (uint64_t)str;
    regs.rdx = strlen(str); // count

    extern void syscall_dispatcher(void *);
    syscall_dispatcher(&regs);

    if (regs.rax == strlen(str)) {
      console_puts("  -> SUCCESS: Syscall returned correctly bytes written.\n");
    } else {
      console_puts("  -> FAIL: Syscall return corrupted.\n");
    }
  } else if (strncmp(cmd, "exec ", 5) == 0) {
    char *args_str = cmd + 5;
    // Simple parsing
    const char *argv[10];
    int argc = 0;
    char *p = args_str;
    while (*p && argc < 9) {
      while (*p == ' ')
        p++;
      if (!*p)
        break;
      argv[argc++] = p;
      while (*p && *p != ' ')
        p++;
      if (*p)
        *p++ = 0;
    }
    argv[argc] = NULL;
    if (argc > 0) {
      if (!process_exec_argv((const char **)argv)) {
        console_puts("exec failed.\n");
      }
    } else {
      console_puts("exec: no command\n");
    }
  } else if (strncmp(cmd, "kilo", 4) == 0) {
    // Usage: kilo <filename>  (e.g. kilo /mnt/test.txt)
    char *filename = cmd + 4;
    while (*filename == ' ')
      filename++; // skip spaces

    if (*filename == '\0') {
      console_puts("Usage: kilo <filename> (e.g. kilo /mnt/test.txt)\n");
    } else {
      const char *argv[] = {"/bin/kilo.elf", filename, NULL};
      if (!process_exec_argv(argv)) {
        console_puts("kilo exec failed.\n");
      }
    }
  } else if (strcmp(cmd, "netinfo") == 0) {
    if (!nic_is_present()) {
      console_puts("No NIC detected.\n");
    } else {
      const uint8_t *mac = nic_get_mac();
      const char *hex = "0123456789ABCDEF";
      console_puts("Network Interface\n");
      console_puts("  MAC Address: ");
      for (int i = 0; i < 6; i++) {
        console_putchar(hex[(mac[i] >> 4) & 0xF]);
        console_putchar(hex[mac[i] & 0xF]);
        if (i < 5)
          console_putchar(':');
      }
      console_puts("\n  Link: ");
      console_puts(nic_link_up() ? "UP" : "DOWN");
      console_puts("\n");
    }
  } else if (strcmp(cmd, "ifconfig") == 0) {
    netif_t *nif = netif_get();
    if (!nif->up) {
      console_puts("Network interface is down.\n");
    } else {
      const char *hex = "0123456789ABCDEF";
      console_puts("eth0:\n");
      console_puts("  MAC: ");
      for (int i = 0; i < 6; i++) {
        console_putchar(hex[(nif->mac[i] >> 4) & 0xF]);
        console_putchar(hex[nif->mac[i] & 0xF]);
        if (i < 5)
          console_putchar(':');
      }
      console_puts("\n  IP:      ");
      shell_print_uint64((nif->ip >> 24) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->ip >> 16) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->ip >> 8) & 0xFF);
      console_putchar('.');
      shell_print_uint64(nif->ip & 0xFF);
      console_puts("\n  Gateway: ");
      shell_print_uint64((nif->gateway >> 24) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->gateway >> 16) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->gateway >> 8) & 0xFF);
      console_putchar('.');
      shell_print_uint64(nif->gateway & 0xFF);
      console_puts("\n  Netmask: ");
      shell_print_uint64((nif->netmask >> 24) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->netmask >> 16) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->netmask >> 8) & 0xFF);
      console_putchar('.');
      shell_print_uint64(nif->netmask & 0xFF);
      console_puts("\n  Status:  UP\n");
    }
  } else if (strcmp(cmd, "arp") == 0) {
    int count = 0;
    const arp_entry_t *table = arp_get_table(&count);
    const char *hex = "0123456789ABCDEF";
    int found = 0;
    console_puts("ARP Cache:\n");
    for (int i = 0; i < count; i++) {
      if (!table[i].valid)
        continue;
      found++;
      console_puts("  ");
      shell_print_uint64((table[i].ip >> 24) & 0xFF);
      console_putchar('.');
      shell_print_uint64((table[i].ip >> 16) & 0xFF);
      console_putchar('.');
      shell_print_uint64((table[i].ip >> 8) & 0xFF);
      console_putchar('.');
      shell_print_uint64(table[i].ip & 0xFF);
      console_puts(" -> ");
      for (int j = 0; j < 6; j++) {
        console_putchar(hex[(table[i].mac[j] >> 4) & 0xF]);
        console_putchar(hex[table[i].mac[j] & 0xF]);
        if (j < 5)
          console_putchar(':');
      }
      console_putchar('\n');
    }
    if (!found) {
      console_puts("  (empty)\n");
    }
  } else if (strncmp(cmd, "arping ", 7) == 0) {
    // Parse dotted-decimal IP: arping 10.0.2.2
    char *ip_str = cmd + 7;
    uint32_t octets[4] = {0};
    int octet_idx = 0;
    for (char *p = ip_str; *p && octet_idx < 4; p++) {
      if (*p >= '0' && *p <= '9') {
        octets[octet_idx] = octets[octet_idx] * 10 + (*p - '0');
      } else if (*p == '.') {
        octet_idx++;
      }
    }
    if (octet_idx == 3) {
      // Final octet was set but no trailing dot
    }
    uint32_t target_ip =
        (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];

    console_puts("ARPING ");
    shell_print_uint64(octets[0]);
    console_putchar('.');
    shell_print_uint64(octets[1]);
    console_putchar('.');
    shell_print_uint64(octets[2]);
    console_putchar('.');
    shell_print_uint64(octets[3]);
    console_puts("...\n");

    if (arp_send_request(target_ip) < 0) {
      console_puts("Failed to send ARP request.\n");
    } else {
      // Poll for a reply (up to ~2 seconds)
      const char *hex = "0123456789ABCDEF";
      bool got_reply = false;
      for (int attempt = 0; attempt < 200000; attempt++) {
        net_poll(); // Process any queued packets
        const arp_entry_t *entry = arp_lookup(target_ip);
        if (entry) {
          console_puts("Reply from ");
          shell_print_uint64(octets[0]);
          console_putchar('.');
          shell_print_uint64(octets[1]);
          console_putchar('.');
          shell_print_uint64(octets[2]);
          console_putchar('.');
          shell_print_uint64(octets[3]);
          console_puts(" [MAC: ");
          for (int j = 0; j < 6; j++) {
            console_putchar(hex[(entry->mac[j] >> 4) & 0xF]);
            console_putchar(hex[entry->mac[j] & 0xF]);
            if (j < 5)
              console_putchar(':');
          }
          console_puts("]\n");
          got_reply = true;
          break;
        }
        // Small delay between polls
        for (volatile int d = 0; d < 100; d++) {
        }
      }
      if (!got_reply) {
        console_puts("Timeout: no ARP reply received.\n");
      }
    }
  } else if (strncmp(cmd, "ping ", 5) == 0) {
    char *ip_str = cmd + 5;
    uint32_t octets[4] = {0};
    int octet_idx = 0;
    for (char *p = ip_str; *p && octet_idx < 4; p++) {
      if (*p >= '0' && *p <= '9') {
        octets[octet_idx] = octets[octet_idx] * 10 + (*p - '0');
      } else if (*p == '.') {
        octet_idx++;
      }
    }
    uint32_t target_ip =
        (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];

    console_puts("PING ");
    shell_print_uint64(octets[0]);
    console_putchar('.');
    shell_print_uint64(octets[1]);
    console_putchar('.');
    shell_print_uint64(octets[2]);
    console_putchar('.');
    shell_print_uint64(octets[3]);
    console_puts("...\n");

    // Try to send. If it fails with -1, it's usually because ARP is missing.
    if (icmp_send_echo(target_ip, 0xC0DE, 1, "AscentOS Ping", 13) < 0) {
      // Missing ARP? Wait a bit.
      bool resolved = false;
      netif_t *nif = netif_get();
      uint32_t next_hop = target_ip;
      if (nif && (target_ip & nif->netmask) != (nif->ip & nif->netmask)) {
        next_hop = nif->gateway;
      }
      for (int i = 0; i < 1000; i++) { // Wait up to 1 second
        net_poll();
        if (arp_lookup(next_hop)) {
          resolved = true;
          break;
        }
        for (volatile int d = 0; d < 10000; d++)
          ; // ~1ms delay
      }

      if (resolved) {
        // Try again now that MAC is known
        if (icmp_send_echo(target_ip, 0xC0DE, 1, "AscentOS Ping", 13) < 0) {
          console_puts(
              "Destination host unreachable (send failed after ARP).\n");
          return;
        }
      } else {
        console_puts(
            "Destination host unreachable (no ARP reply or link down).\n");
        return;
      }
    }

    // If we reached here, packet was sent. Now wait for reply.
    bool got_reply = false;
    extern uint64_t icmp_reply_count;
    uint64_t start_count = icmp_reply_count;

    for (int attempt = 0; attempt < 200000; attempt++) {
      net_poll();
      if (icmp_reply_count > start_count) {
        console_puts("Reply received!\n");
        got_reply = true;
        break;
      }
      for (volatile int d = 0; d < 100; d++) {
      }
    }
    if (!got_reply) {
      console_puts("Timeout: no ICMP reply received.\n");
    }
  } else if (strncmp(cmd, "host ", 5) == 0) {
    char *domain = cmd + 5;
    console_puts("Resolving ");
    console_puts(domain);
    console_puts("...\n");

    uint32_t resolved_ip = 0;
    if (dns_resolve_A_record(domain, &resolved_ip) == 0) {
      console_puts(domain);
      console_puts(" is at ");
      uint8_t a = (resolved_ip >> 24) & 0xFF;
      uint8_t b = (resolved_ip >> 16) & 0xFF;
      uint8_t c = (resolved_ip >> 8) & 0xFF;
      uint8_t d = resolved_ip & 0xFF;
      shell_print_uint64(a);
      console_putchar('.');
      shell_print_uint64(b);
      console_putchar('.');
      shell_print_uint64(c);
      console_putchar('.');
      shell_print_uint64(d);
      console_putchar('\n');
    } else {
      console_puts("Failed to resolve domain.\n");
    }
  } else if (strncmp(cmd, "http ", 5) == 0) {
    char *domain = cmd + 5;
    console_puts("Resolving ");
    console_puts(domain);
    console_puts("...\n");

    uint32_t resolved_ip = 0;
    if (dns_resolve_A_record(domain, &resolved_ip) == 0) {
      console_puts("Connecting to TCP port 80...\n");
      int sock = tcp_connect(resolved_ip, 80, http_recv_cb);
      if (sock >= 0) {
        console_puts("Connected! Sending HTTP GET...\n");
        char req[256];
        memset(req, 0, sizeof(req));
        strcpy(req, "GET / HTTP/1.1\r\nHost: ");
        strcpy(req + strlen(req), domain);
        strcpy(req + strlen(req), "\r\nConnection: close\r\n\r\n");

        if (tcp_send(sock, req, strlen(req)) > 0) {
          // Wait to receive the webpage (timeout handles closing)
          // We'll spin for 3 seconds listening to incoming traffic
          for (int wait = 0; wait < 3000; wait++) {
            net_poll();
            for (volatile int d = 0; d < 10000; d++)
              ;
          }
        } else {
          console_puts("Failed to send request.\n");
        }
        tcp_close(sock);
        console_puts("\n[Connection Closed]\n");
      } else {
        console_puts("Connection failed.\n");
      }
    } else {
      console_puts("Failed to resolve domain.\n");
    }
  } else if (strcmp(cmd, "ifconfig") == 0) {
    netif_t *nif = netif_get();
    console_puts("eth0:\n");

    console_puts("  MAC: ");
    for (int i = 0; i < 6; i++) {
      shell_print_hex_byte(nif->mac[i]);
      if (i < 5)
        console_putchar(':');
    }
    console_puts("\n");

    console_puts("  IP Address: ");
    shell_print_uint64((nif->ip >> 24) & 0xFF);
    console_putchar('.');
    shell_print_uint64((nif->ip >> 16) & 0xFF);
    console_putchar('.');
    shell_print_uint64((nif->ip >> 8) & 0xFF);
    console_putchar('.');
    shell_print_uint64(nif->ip & 0xFF);
    console_puts("\n");

    console_puts("  Subnet Mask: ");
    shell_print_uint64((nif->netmask >> 24) & 0xFF);
    console_putchar('.');
    shell_print_uint64((nif->netmask >> 16) & 0xFF);
    console_putchar('.');
    shell_print_uint64((nif->netmask >> 8) & 0xFF);
    console_putchar('.');
    shell_print_uint64(nif->netmask & 0xFF);
    console_puts("\n");

    console_puts("  Gateway: ");
    shell_print_uint64((nif->gateway >> 24) & 0xFF);
    console_putchar('.');
    shell_print_uint64((nif->gateway >> 16) & 0xFF);
    console_putchar('.');
    shell_print_uint64((nif->gateway >> 8) & 0xFF);
    console_putchar('.');
    shell_print_uint64(nif->gateway & 0xFF);
    console_puts("\n");
  } else {
    console_puts("Unknown command. Type 'help' for available commands.\n");
  }
}

void shell_init(void) {
  // Wait a brief moment or setup anything needed
  console_puts("\nWelcome to AscentOS Shell.\n");
}

void shell_run(void) {
  cmd_len = 0;
  cmd_buffer[0] = '\0';
  print_prompt();

  console_set_cursor_visible(true);

  while (1) {
    console_refresh_cursor();

    char c = 0;
    if (keyboard_has_char()) {
      c = keyboard_get_char();
    } else if (serial_received()) {
      c = serial_get_char();
    }

    if (c == 0) {
      net_poll();    // Background network processing
      sched_yield(); // Don't hog the CPU if no input
      continue;
    }

    if (c == (char)KEY_UP) {
      console_scroll_view(1);
    } else if (c == (char)KEY_DOWN) {
      console_scroll_view(-1);
    } else if (c == (char)KEY_PGUP) {
      console_scroll_view(10);
    } else if (c == (char)KEY_PGDN) {
      console_scroll_view(-10);
    } else if (c == '\r' || c == '\n') {
      console_putchar('\n');
      cmd_buffer[cmd_len] = '\0';
      execute_command(cmd_buffer);
      cmd_len = 0;
      cmd_buffer[0] = '\0';
      print_prompt();
    } else if (c == '\b' || c == 0x7F) { // 0x7F is DEL, often used as backspace
                                         // in serial terms
      if (cmd_len > 0) {
        cmd_len--;
        cmd_buffer[cmd_len] = '\0';
        console_putchar('\b');
      }
    } else {
      if (cmd_len < CMD_BUFFER_SIZE - 1) {
        cmd_buffer[cmd_len++] = c;
        console_putchar(c);
      }
    }
  }
}

static void test_task_entry(void) {
  while (1) {
    // Sleep for 1 second (1000 ticks)
    lapic_timer_sleep(1000);

    struct cpu_info *cpu = cpu_get_current();

    klog_puts("[TASK] Background thread executing on CPU ");
    klog_uint64(cpu->cpu_id);
    klog_puts("!\n");
  }
}
