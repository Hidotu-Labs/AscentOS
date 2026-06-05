#include "isr.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "apic/lapic.h"
#include "msr.h"
#include "pic.h"

const char *exception_messages[] = {"Division By Zero",
                                    "Debug",
                                    "Non Maskable Interrupt",
                                    "Breakpoint",
                                    "Into Detected Overflow",
                                    "Out of Bounds",
                                    "Invalid Opcode",
                                    "No Coprocessor",
                                    "Double Fault",
                                    "Coprocessor Segment Overrun",
                                    "Bad TSS",
                                    "Segment Not Present",
                                    "Stack Fault",
                                    "General Protection Fault",
                                    "Page Fault",
                                    "Unknown Interrupt",
                                    "Coprocessor Fault",
                                    "Alignment Check",
                                    "Machine Check",
                                    "SIMD Floating-Point Exception",
                                    "Virtualization Exception",
                                    "Control Protection Exception",
                                    "Reserved",
                                    "Reserved",
                                    "Reserved",
                                    "Reserved",
                                    "Reserved",
                                    "Reserved",
                                    "Hypervisor Injection Exception",
                                    "VMM Communication Exception",
                                    "Security Exception",
                                    "Reserved"};

// Low-level output helpers

static void print_hex(uint64_t value) {
  const char *hex_chars = "0123456789ABCDEF";
  console_puts("0x");
  for (int i = 15; i >= 0; i--) {
    console_putchar(hex_chars[(value >> (i * 4)) & 0xF]);
  }
}

static void print_hex8(uint8_t value) {
  const char *hex_chars = "0123456789ABCDEF";
  console_puts("0x");
  console_putchar(hex_chars[(value >> 4) & 0xF]);
  console_putchar(hex_chars[value & 0xF]);
}

static void print_dec(uint64_t value) {
  if (value == 0) {
    console_putchar('0');
    return;
  }
  char buf[21];
  int i = 0;
  while (value > 0) {
    buf[i++] = '0' + (value % 10);
    value /= 10;
  }
  for (int j = i - 1; j >= 0; j--)
    console_putchar(buf[j]);
}

static void print_reg_line(const char *name, uint64_t value) {
  console_puts(name);
  console_puts(": ");
  print_hex(value);
  console_puts("\n");
}

static void print_yes_no(const char *name, bool value) {
  console_puts(name);
  console_puts("=");
  console_puts(value ? "1 " : "0 ");
}

static bool is_canonical_addr(uint64_t vaddr) {
  uint64_t high = vaddr >> 48;
  return (high == 0x0000ULL) || (high == 0xFFFFULL);
}

// RFLAGS decoder

static void print_rflags_decoded(uint64_t rflags) {
  console_puts("RFLAGS: ");
  print_hex(rflags);
  console_puts("\n  Flags: ");
  print_yes_no("CF", (rflags >> 0) & 1);
  print_yes_no("PF", (rflags >> 2) & 1);
  print_yes_no("AF", (rflags >> 4) & 1);
  print_yes_no("ZF", (rflags >> 6) & 1);
  print_yes_no("SF", (rflags >> 7) & 1);
  print_yes_no("TF", (rflags >> 8) & 1);
  print_yes_no("IF", (rflags >> 9) & 1);
  print_yes_no("DF", (rflags >> 10) & 1);
  print_yes_no("OF", (rflags >> 11) & 1);
  console_puts("\n  IOPL=");
  print_dec((rflags >> 12) & 0x3);
  print_yes_no(" NT", (rflags >> 14) & 1);
  print_yes_no("RF", (rflags >> 16) & 1);
  print_yes_no("VM", (rflags >> 17) & 1);
  print_yes_no("AC", (rflags >> 18) & 1);
  print_yes_no("VIF", (rflags >> 19) & 1);
  print_yes_no("VIP", (rflags >> 20) & 1);
  print_yes_no("ID", (rflags >> 21) & 1);
  console_puts("\n");
}

// Control register diagnostics

static void print_cr_state(void) {
  uint64_t cr0, cr3, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  console_puts("\nCONTROL REGISTERS:\n");

  console_puts("CR0: ");
  print_hex(cr0);
  console_puts("\n  ");
  print_yes_no("PE", (cr0 >> 0) & 1);
  print_yes_no("WP", (cr0 >> 16) & 1);
  print_yes_no("PG", (cr0 >> 31) & 1);
  console_puts("\n");

  console_puts("CR3: ");
  print_hex(cr3);
  console_puts("\n  PCID=");
  print_hex(cr3 & 0xFFF);
  console_puts(" PML4_PHYS=");
  print_hex(cr3 & ~0xFFFULL);
  console_puts("\n");

  console_puts("CR4: ");
  print_hex(cr4);
  console_puts("\n  ");
  print_yes_no("PSE", (cr4 >> 4) & 1);
  print_yes_no("PAE", (cr4 >> 5) & 1);
  print_yes_no("PGE", (cr4 >> 7) & 1);
  print_yes_no("SMEP", (cr4 >> 20) & 1);
  print_yes_no("SMAP", (cr4 >> 21) & 1);
  print_yes_no("PKE", (cr4 >> 22) & 1);
  print_yes_no("CET", (cr4 >> 23) & 1);
  console_puts("\n");
}

// GP fault decoder

static void print_gp_error_details(uint64_t err_code) {
  console_puts("GP_ERR_DETAILS: ");
  if (err_code == 0) {
    console_puts("none (not selector-related)\n");
    return;
  }
  const char *tbl_names[] = {"GDT", "IDT", "LDT", "IDT"};
  uint8_t ext = err_code & 0x1;
  uint8_t tbl = (err_code >> 1) & 0x3;
  uint16_t idx = (err_code >> 3) & 0x1FFF;
  console_puts("ext=");
  print_hex8(ext);
  console_puts(" tbl=");
  console_puts(tbl_names[tbl]);
  console_puts(" idx=");
  print_dec(idx);
  console_puts(" (");
  print_hex(err_code);
  console_puts(")\n");
}

// PF fault error code decoder

static void print_pf_error_details(uint64_t err_code) {
  bool p = (err_code >> 0) & 1;    /* page present */
  bool w = (err_code >> 1) & 1;    /* write */
  bool u = (err_code >> 2) & 1;    /* user mode */
  bool rsvd = (err_code >> 3) & 1; /* reserved bit set in PTE */
  bool i = (err_code >> 4) & 1;    /* instruction fetch */
  bool pk = (err_code >> 5) & 1;   /* protection key */
  bool ss = (err_code >> 6) & 1;   /* shadow stack */

  console_puts("PF_ERR_DETAILS: ");
  print_yes_no("P", p);
  print_yes_no("W", w);
  print_yes_no("U", u);
  print_yes_no("RSVD", rsvd);
  print_yes_no("I", i);
  print_yes_no("PK", pk);
  print_yes_no("SS", ss);
  console_puts("\n  Cause: ");
  if (i)
    console_puts("instruction fetch from ");
  else if (w)
    console_puts("write to ");
  else
    console_puts("read from ");
  console_puts(p ? "present page (protection violation)" : "non-present page");
  if (u)
    console_puts(", from user mode");
  else
    console_puts(", from kernel mode");
  if (rsvd)
    console_puts(" [RSVD BIT IN PTE - possible corruption]");
  if (pk)
    console_puts(" [protection-key violation]");
  if (ss)
    console_puts(" [shadow stack violation]");
  console_puts("\n");
}

static void analyze_pte_corruption(uint64_t pte) {
  const uint64_t PA_MASK = 0x000FFFFFFFFFF000ULL;
  const uint64_t LOW_MASK = 0xFFFULL;               /* bits [11:0] */
  const uint64_t HIGH_MASK = 0x7FF0000000000000ULL; /* bits [62:52] */

  uint64_t low_bits = pte & LOW_MASK;
  uint64_t high_bits = pte & HIGH_MASK;
  uint64_t pa = pte & PA_MASK;

  bool present = (pte >> 0) & 1;
  bool rw = (pte >> 1) & 1;
  bool us = (pte >> 2) & 1;
  bool dirty = (pte >> 6) & 1;
  bool global = (pte >> 8) & 1;
  bool nx = (pte >> 63) & 1;

  console_puts("  PTE_CORRUPTION_ANALYSIS:\n");
  console_puts("    raw=");
  print_hex(pte);
  console_puts("\n    PA field=");
  print_hex(pa);
  console_puts("\n    low_bits[11:0]=");
  print_hex(low_bits);
  console_puts(" high_bits[62:52]=");
  print_hex(high_bits);
  console_puts("\n");

  if (high_bits != 0) {
    console_puts("    WARN: bits [62:52] are non-zero (");
    print_hex(high_bits);
    console_puts(") -- reserved or OS-specific bits set; likely corruption\n");
  }

  uint32_t lo32 = (uint32_t)(pte & 0xFFFFFFFFULL);
  uint32_t hi32 = (uint32_t)(pte >> 32);
  if (hi32 != 0 && hi32 != 0xFFFFFFFF) {
    uint32_t diff = lo32 ^ hi32;
    if (__builtin_popcount(diff) <= 4) {
      console_puts("    WARN: upper and lower 32-bit halves differ by only ");
      print_dec(__builtin_popcount(diff));
      console_puts(" bit(s) (lo=");
      print_hex(lo32);
      console_puts(" hi=");
      print_hex(hi32);
      console_puts(
          ") -- looks like a 32-bit value replicated into both halves\n");
    }
  }

  if (!present && dirty)
    console_puts("    WARN: D=1 but P=0 (dirty non-present page)\n");
  if (!present && global)
    console_puts("    WARN: G=1 but P=0 (global non-present page)\n");
  if (!rw && dirty)
    console_puts("    NOTE: D=1 but RW=0 (was writable, now read-only)\n");
  if (nx && !us && !present)
    console_puts("    NOTE: NX + kernel + non-present\n");

  if (high_bits == 0 && lo32 == 0 && hi32 == 0)
    console_puts("    PTE is completely zero (was never mapped or was "
                 "explicitly cleared)\n");
}

static void print_paging_entry_flags(uint64_t entry, bool is_leaf,
                                     bool is_pde) {
  print_yes_no("P", (entry & (1ULL << 0)) != 0);
  print_yes_no("RW", (entry & (1ULL << 1)) != 0);
  print_yes_no("US", (entry & (1ULL << 2)) != 0);
  print_yes_no("PWT", (entry & (1ULL << 3)) != 0);
  print_yes_no("PCD", (entry & (1ULL << 4)) != 0);
  print_yes_no("A", (entry & (1ULL << 5)) != 0);
  if (is_leaf || is_pde)
    print_yes_no("D", (entry & (1ULL << 6)) != 0);
  if (is_pde)
    print_yes_no("PS", (entry & (1ULL << 7)) != 0);
  else if (is_leaf)
    print_yes_no("PAT", (entry & (1ULL << 7)) != 0);
  if (is_leaf)
    print_yes_no("G", (entry & (1ULL << 8)) != 0);
  print_yes_no("NX", (entry & (1ULL << 63)) != 0);
  console_puts("\n");
}

static void print_entry_summary(const char *name, size_t idx, uint64_t entry,
                                bool is_leaf, bool is_pde) {
  console_puts("  ");
  console_puts(name);
  console_puts("[");
  print_hex(idx);
  console_puts("] raw=");
  print_hex(entry);
  console_puts(" phys_base=");
  print_hex(entry & PAGE_MASK);
  console_puts("\n    flags: ");
  print_paging_entry_flags(entry, is_leaf, is_pde);
}

static void print_neighbor_entries(const char *label, uint64_t *table,
                                   size_t index) {
  size_t start = (index > 1) ? index - 1 : 0;
  size_t end = (index < 510) ? index + 1 : 511;

  console_puts("    ");
  console_puts(label);
  console_puts(" neighborhood:\n");
  for (size_t i = start; i <= end; i++) {
    console_puts("      [");
    print_hex(i);
    console_puts("] = ");
    print_hex(table[i]);
    if (i == index)
      console_puts("  <target>");
    console_puts("\n");
  }
}

static void print_pf_walk(uint64_t cr2) {
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t *pml4_phys = vmm_get_active_pml4();
  uint64_t *pml4 = (uint64_t *)((uint64_t)pml4_phys + hhdm);

  size_t pml4_i = (cr2 >> 39) & 0x1FF;
  size_t pdpt_i = (cr2 >> 30) & 0x1FF;
  size_t pd_i = (cr2 >> 21) & 0x1FF;
  size_t pt_i = (cr2 >> 12) & 0x1FF;
  uint64_t page_off = cr2 & 0xFFF;

  console_puts("PF_WALK:\n");
  uint64_t pml4e = pml4[pml4_i];
  print_entry_summary("PML4E", pml4_i, pml4e, false, false);
  if (!(pml4e & 1))
    return;

  uint64_t *pdpt = (uint64_t *)((pml4e & PAGE_MASK) + hhdm);
  uint64_t pdpte = pdpt[pdpt_i];
  print_entry_summary("PDPTE", pdpt_i, pdpte, false, false);
  if (!(pdpte & 1) || (pdpte & (1ULL << 7)))
    return;

  uint64_t *pd = (uint64_t *)((pdpte & PAGE_MASK) + hhdm);
  uint64_t pde = pd[pd_i];
  print_entry_summary("PDE", pd_i, pde, false, true);
  if (!(pde & 1) || (pde & (1ULL << 7)))
    return;

  uint64_t *pt = (uint64_t *)((pde & PAGE_MASK) + hhdm);
  uint64_t pte = pt[pt_i];
  print_entry_summary("PTE", pt_i, pte, true, false);
}

static void print_user_stack_words(uint64_t user_rsp, int words) {
  uint64_t *pml4 = vmm_get_active_pml4();
  console_puts("USER_STACK_TOP:\n");
  for (int i = 0; i < words; i++) {
    uint64_t addr = user_rsp + ((uint64_t)i * sizeof(uint64_t));
    console_puts("  [");
    print_hex(addr);
    console_puts("] = ");
    if (vmm_virt_to_phys(pml4, addr) == 0) {
      console_puts("<unmapped>\n");
      continue;
    }
    print_hex(*(volatile uint64_t *)addr);
    console_puts("\n");
  }
}

static void print_context_summary(struct registers *regs) {
  uint8_t cpl = regs->cs & 0x3;
  console_puts("CONTEXT: ");
  if (cpl == 0)
    console_puts("kernel (ring 0)");
  else
    console_puts("user   (ring 3)");
  console_puts("  CS=");
  print_hex(regs->cs);
  console_puts("  SS=");
  print_hex(regs->ss);
  console_puts("\n");
}

static isr_t interrupt_handlers[256] = {0};
static bool apic_mode = false;

void isr_set_apic_mode(bool enabled) { apic_mode = enabled; }

void register_interrupt_handler(uint8_t n, isr_t handler) {
  interrupt_handlers[n] = handler;
}

// Send End-of-Interrupt signal after handler completes.
// CRITICAL: This must be called AFTER the handler returns, not during.
// For shared IRQs (multiple handlers on one IRQ), this ensures all handlers
// run before we tell the APIC we're done. If EOI were sent mid-dispatch,
// the APIC would accept a new interrupt before all shared handlers complete,
// potentially causing handlers to miss their turn or see corrupted state.
static void send_eoi(struct registers *regs) {
  if (regs->int_no == 255)
    return;
  if (regs->int_no < 32)
    return;
  if (apic_mode)
    lapic_send_eoi();
  else if (regs->int_no <= 47)
    pic_send_eoi(regs->int_no - 32);
}

// Exception Handling & Signals

static void isr_panic(struct registers *regs, const char *msg) {
  console_clear();
  console_puts("==================== KERNEL PANIC ====================\n");
  console_puts(msg);
  console_puts("  [INT ");
  print_dec(regs->int_no);
  console_puts("]\n");

  if (regs->int_no < 32) {
    console_puts("Exception: ");
    console_puts(exception_messages[regs->int_no]);
    console_puts("\n");
  }

  console_puts("ERR_CODE: ");
  print_hex(regs->err_code);
  console_puts("\n\n");

  print_context_summary(regs);
  console_puts("RIP: ");
  print_hex(regs->rip);
  console_puts(" RSP: ");
  print_hex(regs->rsp);
  console_puts("\n");

  if (regs->int_no == 13) {
    print_gp_error_details(regs->err_code);
  } else if (regs->int_no == 14) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    console_puts("CR2: ");
    print_hex(cr2);
    console_puts("\n");
    print_pf_error_details(regs->err_code);
    print_pf_walk(cr2);
  }

  print_cr_state();

  console_puts("\nSystem Halted.\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

static void isr_report_user_fault(struct registers *regs, int sig,
                                  uint64_t addr) {
  (void)addr;
  struct thread *current = sched_get_current();
  if (current) {
    current->pending_signals |= (1ULL << (sig - 1));
  } else {
    isr_panic(regs, "User fault with no thread context");
  }
}

static void page_fault_handler(struct registers *regs) {
  uint64_t cr2;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

  if (vmm_handle_page_fault(cr2, regs->err_code, regs) != 0) {
    if ((regs->cs & 0x3) == 0x3) {
      isr_report_user_fault(regs, SIGSEGV, cr2);
    } else {
      isr_panic(regs, "Unhandled Kernel Page Fault");
    }
  }
}

static void gpf_handler(struct registers *regs) {
  if ((regs->cs & 0x3) == 0x3) {
    isr_report_user_fault(regs, SIGSEGV, 0);
  } else {
    isr_panic(regs, "Unhandled General Protection Fault");
  }
}

static void stack_fault_handler(struct registers *regs) {
  if ((regs->cs & 0x3) == 0x3) {
    isr_report_user_fault(regs, SIGSTKFLT, 0);
  } else {
    isr_panic(regs, "Unhandled Stack Fault");
  }
}

void isr_init_exceptions(void) {
  register_interrupt_handler(12, stack_fault_handler);
  register_interrupt_handler(13, gpf_handler);
  register_interrupt_handler(14, page_fault_handler);
}

void isr_handler(struct registers *regs) {
  if (interrupt_handlers[regs->int_no] != 0) {
    isr_t handler = interrupt_handlers[regs->int_no];
    handler(regs);

    if ((regs->cs & 0x3) == 0x3) {
      signal_deliver(regs);
    }

    send_eoi(regs);
    return;
  }

  if (regs->int_no >= 32) {
    if ((regs->cs & 0x3) == 0x3) {
      signal_deliver(regs);
    }
    send_eoi(regs);
    return;
  }

  isr_panic(regs, "Unhandled CPU Exception");
}