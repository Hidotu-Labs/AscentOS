// ── Signal Syscalls: rt_sigaction, rt_sigprocmask ───────────────────────────
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct sigframe {
  struct registers regs;
  uint64_t mask;
} __attribute__((packed));

// ── rt_sigaction: Set or get signal action ──────────────────────────────────
static uint64_t sys_rt_sigaction(uint64_t signum, uint64_t act_ptr,
                                 uint64_t oldact_ptr, uint64_t sigsetsize,
                                 uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (signum > 64 || signum < 1 || sigsetsize != 8)
    return (uint64_t)-22;

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;
  uint64_t idx = signum - 1;

  if (oldact_ptr) {
    if (!vmm_is_user_addr_range_valid(oldact_ptr, sizeof(struct k_sigaction)))
      return (uint64_t)-14; // EFAULT
    struct k_sigaction *old = (struct k_sigaction *)oldact_ptr;
    *old = current->signal_handlers[idx];
  }

  if (act_ptr) {
    if (!vmm_is_user_addr_range_valid(act_ptr, sizeof(struct k_sigaction)))
      return (uint64_t)-14;
    struct k_sigaction *new = (struct k_sigaction *)act_ptr;
    if (signum == SIGKILL || signum == SIGSTOP)
      return (uint64_t)-22;
    current->signal_handlers[idx] = *new;
  }
  return 0;
}

// ── rt_sigprocmask: Set or get signal mask ──────────────────────────────────
static uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_ptr,
                                   uint64_t oldset_ptr, uint64_t sigsetsize,
                                   uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  if (sigsetsize != 8)
    return (uint64_t)-22;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  if (oldset_ptr) {
    if (!vmm_is_user_addr_range_valid(oldset_ptr, sizeof(uint64_t)))
      return (uint64_t)-14;
    *(uint64_t *)oldset_ptr = current->signal_mask;
  }

  if (set_ptr) {
    if (!vmm_is_user_addr_range_valid(set_ptr, sizeof(uint64_t)))
      return (uint64_t)-14;
    uint64_t newset = *(uint64_t *)set_ptr;
    newset &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

    switch (how) {
    case SIG_BLOCK:
      current->signal_mask |= newset;
      break;
    case SIG_UNBLOCK:
      current->signal_mask &= ~newset;
      break;
    case SIG_SETMASK:
      current->signal_mask = newset;
      break;
    default:
      return (uint64_t)-22;
    }
  }
  return 0;
}

// ── sigreturn ──────────────────────────────────────────────────────────────
static uint64_t sys_rt_sigreturn(struct syscall_regs *sregs) {
  struct thread *current = sched_get_current();
  // Frame is on user stack.
  struct sigframe *frame = (struct sigframe *)sregs->rsp;
  if (!vmm_is_user_addr_range_valid((uint64_t)frame, sizeof(struct sigframe))) {
    klog_puts("[SIGNAL] sigreturn: invalid frame pointer\n");
    process_do_exit(11); // SIGSEGV
  }

  current->signal_mask = frame->mask;

  // Restore state into syscall_regs
  sregs->rax = frame->regs.rax;
  sregs->rbx = frame->regs.rbx;
  sregs->rbp = frame->regs.rbp;
  sregs->r12 = frame->regs.r12;
  sregs->r13 = frame->regs.r13;
  sregs->r14 = frame->regs.r14;
  sregs->r15 = frame->regs.r15;
  sregs->rdi = frame->regs.rdi;
  sregs->rsi = frame->regs.rsi;
  sregs->rdx = frame->regs.rdx;
  sregs->r10 = frame->regs.r10;
  sregs->r8 = frame->regs.r8;
  sregs->r9 = frame->regs.r9;

  sregs->rip = frame->regs.rip;
  sregs->rflags = frame->regs.rflags;
  sregs->rsp = frame->regs.rsp;

  return sregs->rax;
}

// ── Signal Delivery ─────────────────────────────────────────────────────────

void signal_deliver(struct registers *regs) {
  struct thread *current = sched_get_current();
  if (!current)
    return;

  uint64_t pending = current->pending_signals & ~current->signal_mask;
  if (!pending)
    return;

  int sig = 0;
  for (int i = 0; i < 64; i++) {
    if (pending & (1ULL << i)) {
      sig = i + 1;
      break;
    }
  }
  if (!sig)
    return;

  current->pending_signals &= ~(1ULL << (sig - 1));
  struct k_sigaction *sa = &current->signal_handlers[sig - 1];

  if (sa->sa_handler == (void *)SIG_IGN)
    return;
  if (sa->sa_handler == (void *)SIG_DFL) {
    if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH)
      return;
    klog_puts("[SIGNAL] Default action (terminate) for sig ");
    klog_uint64(sig);
    klog_puts("\n");
    process_do_exit(128 + sig);
    return;
  }

  // Push frame to user stack
  uint64_t rsp = regs->rsp;
  rsp -= sizeof(struct sigframe);
  rsp &= ~0xFULL;

  if (!vmm_is_user_addr_range_valid(rsp, sizeof(struct sigframe))) {
    klog_puts("[SIGNAL] Stack overflow/invalid during delivery\n");
    process_do_exit(11); // SIGSEGV
  }

  struct sigframe *frame = (struct sigframe *)rsp;
  frame->regs = *regs;
  frame->mask = current->signal_mask;

  regs->rip = (uint64_t)sa->sa_handler;
  regs->rdi = sig;

  // Set up return
  rsp -= 8;
  if (!vmm_is_user_addr_range_valid(rsp, 8)) {
    klog_puts("[SIGNAL] Stack overflow during return setup\n");
    process_do_exit(11);
  }

  if (sa->sa_flags & 0x04000000) { // SA_RESTORER
    *(uint64_t *)rsp = (uint64_t)sa->sa_restorer;
  } else {
    // If no restorer, we'd need a kernel trampoline. We'll warn for now.
    klog_puts("[SIGNAL] No SA_RESTORER for sig ");
    klog_uint64(sig);
    klog_puts("\n");
  }
  regs->rsp = rsp;

  if (!(sa->sa_flags & 0x40000000)) { // !SA_NODEFER
    current->signal_mask |= sa->sa_mask | (1ULL << (sig - 1));
  }
}

// Helper to deliver from syscall context
void signal_deliver_syscall(struct syscall_regs *sregs) {
  struct registers regs = {0};
  regs.rdi = sregs->rdi;
  regs.rsi = sregs->rsi;
  regs.rdx = sregs->rdx;
  regs.r10 = sregs->r10;
  regs.r8 = sregs->r8;
  regs.r9 = sregs->r9;
  regs.rax = sregs->rax;
  regs.rbx = sregs->rbx;
  regs.rbp = sregs->rbp;
  regs.r12 = sregs->r12;
  regs.r13 = sregs->r13;
  regs.r14 = sregs->r14;
  regs.r15 = sregs->r15;
  regs.rip = sregs->rip;
  regs.rflags = sregs->rflags;
  regs.rsp = sregs->rsp;
  regs.cs = 0x2B;
  regs.ss = 0x23; // Standard user segments

  signal_deliver(&regs);

  // Sync back
  sregs->rdi = regs.rdi;
  sregs->rsi = regs.rsi;
  sregs->rdx = regs.rdx;
  sregs->r10 = regs.r10;
  sregs->r8 = regs.r8;
  sregs->r9 = regs.r9;
  sregs->rax = regs.rax;
  sregs->rbx = regs.rbx;
  sregs->rbp = regs.rbp;
  sregs->r12 = regs.r12;
  sregs->r13 = regs.r13;
  sregs->r14 = regs.r14;
  sregs->r15 = regs.r15;
  sregs->rip = regs.rip;
  sregs->rflags = regs.rflags;
  sregs->rsp = regs.rsp;
}

// ── Other stubs ──────────────────────────────────────────────────────────────

static uint64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)tgid;
  (void)a3;
  (void)a4;
  (void)a5;

  // Validate signal number (1-64 are valid)
  if (sig > 64)
    return (uint64_t)-22; // EINVAL

  // Signal 0 is used for existence check - just return success
  if (sig == 0)
    return 0;

  // Try to find target thread
  struct thread *target = sched_get_thread_by_tid((uint32_t)tid);
  if (!target) {
    // If thread not found by tid, try current thread
    struct thread *current = sched_get_current();
    if (!current)
      return (uint64_t)-3; // ESRCH
    
    // If tid matches current's tid, use current
    if (current->tid == (uint32_t)tid) {
      target = current;
    } else {
      // For now, just succeed silently (GTK doesn't care about actual delivery)
      return 0;
    }
  }

  // Queue signal if we found target
  if (target && sig > 0 && sig <= 64) {
    target->pending_signals |= (1ULL << (sig - 1));
  }

  return 0;
}

typedef struct {
  uint64_t ss_sp;
  uint32_t ss_flags;
  uint32_t __pad;
  uint64_t ss_size;
} stack_t;
static uint64_t sys_sigaltstack(uint64_t ss_ptr, uint64_t old_ss_ptr,
                                uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  if (old_ss_ptr) {
    stack_t *old = (stack_t *)old_ss_ptr;
    old->ss_sp = 0;
    old->ss_flags = 2;
    old->ss_size = 0;
  }
  if (ss_ptr) {
    stack_t *ss = (stack_t *)ss_ptr;
    if (ss->ss_flags & 2)
      return 0;
    if (ss->ss_size < 8192)
      return (uint64_t)-22;
  }
  return 0;
}

static uint64_t sys_sigprocmask(uint64_t how, uint64_t set_ptr,
                                uint64_t oldset_ptr, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return sys_rt_sigprocmask(how, set_ptr, oldset_ptr, 8, 0, 0);
}

// Send signal to all processes in a process group
void signal_send_pgid(uint32_t pgid, int sig) {
  if (sig <= 0 || sig > 64)
    return;
  
  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  
  struct thread *t = global_thread_list;
  while (t) {
    if (t->pgid == pgid) {
      t->pending_signals |= (1ULL << (sig - 1));
    }
    t = t->global_next;
  }
  spinlock_release(&tid_lock);
}

static uint64_t sys_kill(uint64_t pid_val, uint64_t sig, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  int32_t pid = (int32_t)pid_val;
  if (sig > 64)
    return (uint64_t)-22;

  struct thread *current = sched_get_current();
  if (sig == 0) {
    if (pid == 0 || pid == -1) return 0;
    
    bool found = false;
    extern struct thread *global_thread_list;
    extern spinlock_t tid_lock;
    spinlock_acquire(&tid_lock);
    struct thread *t = global_thread_list;
    while (t) {
      if (t->tid == (uint32_t)(pid > 0 ? pid : -pid)) {
        found = true;
        break;
      }
      t = t->global_next;
    }
    spinlock_release(&tid_lock);
    return found ? 0 : (uint64_t)-3; // -ESRCH
  }

  if (pid == 0) {
    signal_send_pgid(current->pgid, (int)sig);
    return 0;
  } else if (pid == -1) {
    // Send to everyone? Not implemented for safety.
    return 0;
  } else if (pid < -1) {
    signal_send_pgid((uint32_t)-pid, (int)sig);
    return 0;
  }

  extern struct thread *global_thread_list;
  extern spinlock_t tid_lock;
  spinlock_acquire(&tid_lock);
  struct thread *t = global_thread_list;
  while (t) {
    if (t->tid == (uint32_t)pid) {
      t->pending_signals |= (1ULL << (sig - 1));
      break;
    }
    t = t->global_next;
  }
  spinlock_release(&tid_lock);
  return 0;
}

void syscall_register_signal(void) {
  syscall_register(SYS_RT_SIGACTION, sys_rt_sigaction);
  syscall_register(SYS_RT_SIGPROCMASK, sys_rt_sigprocmask);
  syscall_register(SYS_SIGPROCMASK, sys_sigprocmask);
  syscall_register_raw(SYS_RT_SIGRETURN, sys_rt_sigreturn);
  syscall_register(SYS_SIGALTSTACK, sys_sigaltstack);
  syscall_register(SYS_TGKILL, sys_tgkill);
  syscall_register(SYS_KILL, sys_kill);
}
