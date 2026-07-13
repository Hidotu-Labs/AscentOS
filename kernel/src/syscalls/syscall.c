#include "syscall.h"
#include "../console/klog.h"
#include "../cpu/msr.h"
#include "../sched/sched.h"
#include <stdint.h>

extern void syscall_entry(void);

static syscall_handler_t syscall_table[MAX_SYSCALL] = {0};
static syscall_raw_handler_t raw_syscall_table[MAX_SYSCALL] = {0};

void syscall_register(int num, syscall_handler_t handler) {
  if (num >= 0 && num < MAX_SYSCALL) {
    syscall_table[num] = handler;
  }
}

void syscall_register_raw(int num, syscall_raw_handler_t handler) {
  if (num >= 0 && num < MAX_SYSCALL) {
    raw_syscall_table[num] = handler;
  }
}

void syscall_dispatcher(struct syscall_regs *regs) {
  struct thread *t = sched_get_current();
  if (t) {
    /*
   klog_puts("[SYSCALL] tid=");
    klog_uint64(t->tid);
    klog_puts(" rax=");
    klog_uint64(regs->rax);
    klog_puts(" rdi=");
    klog_uint64(regs->rdi);
    klog_puts("\n");
    */
  }

  if (regs->rax >= MAX_SYSCALL) {
    klog_puts("\n[SYSCALL] Unimplemented syscall: ");
    klog_uint64(regs->rax);
    klog_puts("\n");
    regs->rax = (uint64_t)-38; // ENOSYS
    return;
  }

  // Check raw handlers first (e.g. fork needs the full register frame)
  if (raw_syscall_table[regs->rax]) {
    syscall_raw_handler_t raw_handler = raw_syscall_table[regs->rax];
    regs->rax = raw_handler(regs);
    return;
  }

  if (!syscall_table[regs->rax]) {
    klog_puts("\n[SYSCALL] Unimplemented syscall: ");
    klog_uint64(regs->rax);
    klog_puts("\n");
    regs->rax = (uint64_t)-38; // ENOSYS
    return;
  }

  uint64_t syscall_num = regs->rax; 
  syscall_handler_t handler = syscall_table[syscall_num];
  regs->rax =
      handler(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, regs->r9);

  if ((int64_t)regs->rax < 0 && (int64_t)regs->rax != -11 &&
      (int64_t)regs->rax != -2) {
    klog_puts("[SYSCALL ERR] syscall ");
    klog_uint64(syscall_num);
    klog_puts(" returned error: ");
    klog_uint64((uint64_t)(-(int64_t)regs->rax)); 
    klog_puts("\n");
  }

  /* Signal frame conversion copies the complete register set. Keep it off the
   * syscall hot path unless this thread can actually deliver a signal. */
  if (t && (t->pending_signals & ~t->signal_mask))
    signal_deliver_syscall(regs);
}

// Core initialization
void syscall_init(void) {
  // Register all syscall subsystems
  syscall_register_io();
  syscall_register_process();
  syscall_register_mm();
  syscall_register_arch();
  syscall_register_signal();
  syscall_register_socket();
  syscall_register_epoll();
  syscall_register_poll();
  syscall_register_shm();
  syscall_register_futex();

  uint64_t efer = rdmsr(IA32_EFER);
  efer |= IA32_EFER_SCE | (1ULL << 11); 
  wrmsr(IA32_EFER, efer);

  uint64_t star = ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32);
  wrmsr(IA32_STAR, star);

  wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);

  wrmsr(IA32_FMASK, 0x200);

  klog_puts("[OK] Syscall Infrastructure (MSRs) initialized.\n");
}
