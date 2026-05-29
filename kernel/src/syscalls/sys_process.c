// ── Process Syscalls: fork, getpid, exit ────────────────────────────────────
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/msr.h"
#include "../drivers/timer/rtc.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "syscall.h"
#include <stdint.h>

extern void mm_reset_mmap_state(struct thread *t);
void vma_list_init(struct vma_list *list);
void vma_list_destroy(struct vma_list *list);

// ── Path Normalization ──────────────────────────────────────────────────────
static void path_normalize(const char *base, const char *rel, char *out) {
  char temp[512] = {0};

  // If relative path starts with '/', it replaces base entirely
  if (rel[0] == '/') {
    strcpy(temp, rel);
  } else {
    strcpy(temp, base);
    if (temp[strlen(temp) - 1] != '/')
      strcat(temp, "/");
    strcat(temp, rel);
  }

  // Collapse elements
  char *collapsed[128];
  int count = 0;

  char *p = temp;
  while (*p) {
    while (*p == '/')
      *p++ = '\0';
    if (!*p)
      break;

    char *elem = p;
    while (*p && *p != '/') {
      p++;
    }
    if (*p == '/') {
      *p = '\0';
      p++;
    }

    if (strcmp(elem, ".") == 0) {
      continue;
    } else if (strcmp(elem, "..") == 0) {
      if (count > 0)
        count--;
    } else {
      if (count < 128) {
        collapsed[count++] = elem;
      }
    }
  }

  // Rebuild out path
  out[0] = '\0';
  for (int i = 0; i < count; i++) {
    strcat(out, "/");
    strcat(out, collapsed[i]);
  }
  if (count == 0)
    strcpy(out, "/");
}

// Used to restart bash session on exit
extern void restart_main_session(void) __attribute__((noreturn));

// Assembly trampoline that restores user registers and sysrets to Ring 3
extern void fork_return_to_userspace(struct syscall_regs *regs)
    __attribute__((noreturn));

// MSR for TLS base registers
#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_FS_BASE 0xC0000100

// ── exit / exit_group (shared) ─────────────────────────────────────────────
void process_do_exit(uint64_t status) __attribute__((noreturn));
void process_do_exit(uint64_t status) {
  struct thread *current = sched_get_current();
  if (current) {
    sched_reparent_children(current);
  }

  if (current && !current->is_main_session) {
    klog_puts("\n[SYSCALL] Process exited with status: ");
    klog_uint64(status);
    klog_puts("\n");
  }

  // If tid_address was set via set_tid_address, clear it (set to 0) to signal
  // exit. We must check if the memory is actually mapped to avoid panicking
  // if the user unmapped it.
  if (current && current->tid_address) {
    if (current->cr3) {
      uint64_t phys = vmm_virt_to_phys((uint64_t *)current->cr3,
                                       (uint64_t)current->tid_address);
      if (phys != 0) {
        *current->tid_address = 0;
      }
    }
  }

  // ── Common Cleanup ────────────────────────────────────────────────────────
  // Close all open file descriptors to prevent resource leaks.
  if (current) {
    for (int i = 0; i < MAX_FDS; i++) {
      if (current->fds[i]) {
        vfs_node_t *node = current->fds[i];
        current->fds[i] = NULL;

        // Dedup: check if same node pointer appears at a higher index
        for (int j = i + 1; j < MAX_FDS; j++) {
          if (current->fds[j] == node) {
            current->fds[j] = NULL;
          }
        }

        vfs_close(node);
      }
    }

    // Switch to kernel CR3. We don't free it here; the scheduler's
    // reaping logic handles reference counting and reclamation of the
    // shared mm_struct and page tables once the thread is reaped.
    if (current->cr3) {
      struct cpu_info *cpu = cpu_get_current();
      __asm__ volatile("mov %0, %%cr3" ::"r"(cpu->kernel_cr3) : "memory");
    }
    if (current->cwd_node) {
      vfs_close(current->cwd_node);
      current->cwd_node = NULL;
    }
  }

  if (current && current->is_forked_child) {
    current->exit_status = (int)status;
    current->state = THREAD_ZOMBIE;

    if (current->parent && current->parent->state == THREAD_BLOCKED) {
      current->parent->state = THREAD_READY;
    }

    // Sleep forever; the parent will reap us.
    while (1) {
      sched_yield();
    }
  }

  // ── Non-forked (main) process exit ──────────────────────────────────────
  extern void restart_main_session(void);
  restart_main_session();
  while (1)
    ;
}

static uint64_t __attribute__((noreturn)) sys_exit(uint64_t status, uint64_t a1,
                                                   uint64_t a2, uint64_t a3,
                                                   uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_do_exit((int)status);
}

static uint64_t __attribute__((noreturn))
sys_exit_group(uint64_t status, uint64_t a1, uint64_t a2, uint64_t a3,
               uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_do_exit(status);
}

static uint64_t sys_set_tid_address(uint64_t tidptr, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return 0;

  // Store the pointer to the user-space TID variable
  // This is used by musl/glibc for thread exit notification
  current->tid_address = (uint64_t *)tidptr;

  // Return the current thread's ID
  return current->tid;
}

static uint64_t sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (current) {
    return current->tid;
  }
  return 0;
}
// ── sys_getcwd ──────────────────────────────────────────────────────────────
static uint64_t sys_getcwd(uint64_t buf_ptr, uint64_t size, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  char *buf = (char *)buf_ptr;
  if (!buf)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  size_t len = strlen(current->cwd_path) + 1;
  if (size < len)
    return (uint64_t)-34; // ERANGE

  memcpy(buf, current->cwd_path, len);
  return len; // Linux getcwd syscall returns length of copied bytes
}

// ── sys_chdir ───────────────────────────────────────────────────────────────
static uint64_t sys_chdir(uint64_t path_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)path_ptr;
  if (!path)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  char new_path[256];
  path_normalize(current->cwd_path, path, new_path);

  // Validate the directory exists!
  // Note: we want an absolute lookup here
  vfs_node_t *node = vfs_resolve_path_at(fs_root, new_path);
  if (!node)
    return (uint64_t)-2; // ENOENT

  if ((node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  // If validation passes, update thread
  if (current->cwd_node)
    vfs_close(current->cwd_node);
  current->cwd_node = node; // vfs_resolve_path_at already opened it

  strncpy(current->cwd_path, new_path, 255);
  current->cwd_path[255] = '\0';

  return 0; // Success
}
extern spinlock_t tid_lock;

#define WNOHANG 1

static uint64_t sys_wait4(uint64_t pid, uint64_t wstatus_ptr, uint64_t options,
                          uint64_t rusage, uint64_t a4, uint64_t a5) {
  (void)rusage;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-10; // ECHILD

  int target_pid = (int)pid;

  while (1) {
    bool has_matching_children = false;
    struct thread *zombie = NULL;

    spinlock_acquire(&tid_lock);
    struct thread *t = current->children;
    while (t) {
      bool matches = false;
      if (target_pid == -1) {
        matches = true;
      } else if (target_pid > 0) {
        if (t->tid == (uint32_t)target_pid)
          matches = true;
      } else if (target_pid == 0) {
        if (t->pgid == current->pgid)
          matches = true;
      } else { // target_pid < -1
        if (t->pgid == (uint32_t)(-target_pid))
          matches = true;
      }

      if (matches) {
        has_matching_children = true;
        if (t->state == THREAD_ZOMBIE) {
          zombie = t;

          // Unlink from parent's children list while holding lock
          if (current->children == zombie) {
            current->children = zombie->sibling_next;
          } else {
            struct thread *p = current->children;
            while (p && p->sibling_next != zombie)
              p = p->sibling_next;
            if (p)
              p->sibling_next = zombie->sibling_next;
          }
          break;
        }
      }
      t = t->sibling_next;
    }
    spinlock_release(&tid_lock);

    if (zombie) {
      if (wstatus_ptr != 0) {
        int *wstatus = (int *)wstatus_ptr;
        *wstatus = (zombie->exit_status & 0xFF) << 8;
      }
      uint32_t reaped_pid = zombie->tid;

      // Fully reap the zombie
      sched_reap_thread(zombie);

      return (uint64_t)reaped_pid;
    }

    if (!has_matching_children) {
      return (uint64_t)-10; // ECHILD
    }

    // WNOHANG: return 0 immediately if no zombie found
    if (options & WNOHANG) {
      return 0;
    }

    // Block and wait for a child to exit
    current->state = THREAD_BLOCKED;
    sched_yield();
  }
}

// ── sys_execve ──────────────────────────────────────────────────────────────
static uint64_t sys_execve(struct syscall_regs *regs) {
  const char **user_argv = (const char **)regs->rsi;
  const char **user_envp = (const char **)regs->rdx;
  const char *user_path = (const char *)regs->rdi;

  if (!user_path)
    return (uint64_t)-14; // EFAULT

  // 1. Copy everything to kernel memory BEFORE switching CR3
  // We need to copy the path, the argv array, and the envp array
  size_t path_len = strlen(user_path);
  char *path = kmalloc(path_len + 1);
  if (!path)
    return (uint64_t)-12;
  memcpy(path, user_path, path_len + 1);

  int argc = 0;
  if (user_argv)
    while (user_argv[argc])
      argc++;
  char **k_argv = kmalloc((argc + 1) * sizeof(char *));
  for (int i = 0; i < argc; i++) {
    size_t len = strlen(user_argv[i]);
    k_argv[i] = kmalloc(len + 1);
    memcpy(k_argv[i], user_argv[i], len + 1);
  }
  k_argv[argc] = NULL;

  int envc = 0;
  if (user_envp)
    while (user_envp[envc])
      envc++;
  char **k_envp = kmalloc((envc + 1) * sizeof(char *));
  for (int i = 0; i < envc; i++) {
    size_t len = strlen(user_envp[i]);
    k_envp[i] = kmalloc(len + 1);
    memcpy(k_envp[i], user_envp[i], len + 1);
  }
  k_envp[envc] = NULL;

  klog_puts("[EXECVE] Executing: ");
  klog_puts(path);
  klog_puts("\n");

  uint64_t *new_pml4 = vmm_create_pml4();
  if (!new_pml4) {
    kfree(path);
    for (int i = 0; i < argc; i++)
      kfree(k_argv[i]);
    kfree(k_argv);
    for (int i = 0; i < envc; i++)
      kfree(k_envp[i]);
    kfree(k_envp);
    return (uint64_t)-12;
  }

  // EARLY CR3 SWITCH:
  struct thread *current = sched_get_current();
  uint64_t old_cr3 = current->cr3;

  // Save the old VMA list before destroying it — we need it to
  // identify MAP_SHARED pages when freeing the old address space.
  struct vma_list old_vmas = current->mm->vmas;
  vma_list_init(&current->mm->vmas); // Reset to empty for the new program

  current->cr3 = (uint64_t)new_pml4;
  __asm__ volatile("mov %0, %%cr3" ::"r"(current->cr3) : "memory");

  // Reset memory management state for the new program
  mm_reset_mmap_state(current);
  current->fs_base = 0;

  // Reset signal handlers to SIG_DFL after exec (POSIX requirement).
  // Stale handler addresses pointing into the old address space would
  // cause a jump to an invalid RIP on the next signal delivery.
  memset(current->signal_handlers, 0, sizeof(current->signal_handlers));
  current->pending_signals = 0;

  elf_info_t elf_info = {0};
  if (!elf_load(path, new_pml4, &elf_info)) {
    klog_puts("[EXECVE] Failed to load ELF\n");
    // Revert CR3
    current->cr3 = old_cr3;
    __asm__ volatile("mov %0, %%cr3" ::"r"(current->cr3) : "memory");
    // Restore old VMA list
    current->mm->vmas = old_vmas;
    kfree(path);
    for (int i = 0; i < argc; i++)
      kfree(k_argv[i]);
    kfree(k_argv);
    for (int i = 0; i < envc; i++)
      kfree(k_envp[i]);
    kfree(k_envp);
    // Note: new_pml4 is leaked if elf_load fails. Fixing that would require
    // vmm_destroy_pml4 or similar, but for now we focus on the reported leak.
    return (uint64_t)-8; // ENOEXEC
  }

  // We are now safely loaded into the new address space!
  uint64_t user_rsp = process_build_initial_stack(
      ASCENTOS_USER_STACK_TOP, path, (const char **)k_argv,
      (const char **)k_envp, &elf_info);

  uint64_t actual_entry =
      elf_info.interp_base ? elf_info.interp_entry : elf_info.entry;

  klog_puts("[EXECVE] Success, returning to user space at ");
  klog_uint64(actual_entry);
  klog_puts("\n");

  // Store the basename of the executable as the thread's comm name
  {
    const char *base = path;
    for (const char *p = path; *p; p++)
      if (*p == '/')
        base = p + 1;
    int ci = 0;
    while (base[ci] && ci < 15) {
      current->comm[ci] = base[ci];
      ci++;
    }
    current->comm[ci] = '\0';
  }

  // Free the old address space (from fork) now that the new one is loaded.
  // We've already switched CR3, so this is safe.
  // Use the saved old_vmas to avoid freeing MAP_SHARED device pages.
  if (old_cr3 != 0) {
    vmm_free_user_pages_vma(old_cr3, &old_vmas);
  }
  // Now destroy the old VMA tree nodes
  vma_list_destroy(&old_vmas);

  // Cleanup kernel-side copies
  for (int i = 0; i < argc; i++)
    kfree(k_argv[i]);
  kfree(k_argv);
  for (int i = 0; i < envc; i++)
    kfree(k_envp[i]);
  kfree(k_envp);
  kfree(path);

  // Reset TLS Bases for this process
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, 0);

  // Set the return registers for the syscall exit handler to jump to!
  regs->rip = actual_entry;
  regs->rsp = user_rsp;

  // We are heavily relying on the fact that `regs` pointer is on the KERNEL
  // stack which is in the higher half and fully mapped identically in
  // `new_pml4`.
  return 0;
}

// ── Fork child kernel thread entry point ────────────────────────────────────
// This function runs as a kernel thread.  When scheduled, it switches to the
// child's page table and sysrets to the user-space address where the parent
// called fork().  RAX will be 0 (child return value).
static void fork_child_entry(void) {
  struct thread *self = sched_get_current();
  struct syscall_regs *child_regs = (struct syscall_regs *)self->fork_ctx;

  klog_puts("[FORK] Child thread ");
  klog_uint64(self->tid);
  klog_puts(" entering userspace\n");

  // Switch to the child's cloned address space
  __asm__ volatile("mov %0, %%cr3" ::"r"(self->cr3) : "memory");

  // Set TSS rsp0 so interrupts and syscalls from Ring 3 use this CPU's
  // kernel stack.
  tss_set_rsp0(cpu_get_current()->stack_top);

  // Restore user TLS bases — child inherits parent's FS_BASE (musl needs TLS)
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, self->fs_base);

  // Jump to userspace — this never returns
  fork_return_to_userspace(child_regs);
}

// ── sys_fork (raw handler — receives full register frame) ───────────────────
uint64_t sys_fork(struct syscall_regs *regs) {
  klog_puts("[FORK] Fork requested\n");

  // 1. Get current parent state
  uint64_t *parent_pml4_phys = vmm_get_active_pml4();
  struct thread *parent = sched_get_current();

  // 2. Clone the user address space with VMA awareness
  //    Shared mappings share physical pages, private mappings get copied
  uint64_t child_cr3 = vmm_clone_user_mappings_vma(
      parent_pml4_phys, (parent && parent->mm) ? &parent->mm->vmas : NULL);
  if (child_cr3 == 0) {
    klog_puts("[FORK] Failed: could not clone address space\n");
    return (uint64_t)(-12); // -ENOMEM
  }

  klog_puts("[FORK] Address space cloned, child CR3: ");
  klog_uint64(child_cr3);
  klog_puts("\n");

  // 3. Allocate and populate the child's saved register state.
  //    RAX = 0 so the child sees fork() returning 0.
  struct syscall_regs *child_regs = kmalloc(sizeof(struct syscall_regs));
  if (!child_regs) {
    klog_puts("[FORK] Failed: OOM for child regs\n");
    if (child_cr3) {
      vmm_free_user_pages_vma(
          child_cr3, (parent && parent->mm) ? &parent->mm->vmas : NULL);
    }
    return (uint64_t)(-12);
  }
  memcpy(child_regs, regs, sizeof(struct syscall_regs));
  child_regs->rax = 0; // Child return value

  // 4. Create a kernel thread for the child process.
  //    sched_create_kernel_thread enqueues it on a CPU's run queue.
  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, cpu_get_current(), false);
  if (!child) {
    kfree(child_regs);
    if (child_cr3) {
      vmm_free_user_pages_vma(
          child_cr3, (parent && parent->mm) ? &parent->mm->vmas : NULL);
    }
    klog_puts("[FORK] Failed: could not create child thread\n");
    return (uint64_t)(-12);
  }

  // Clear or free the default MM allocated by sched_create_kernel_thread
  // as we're about to replace it with a cloned one.
  if (child->mm) {
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
    child->mm = NULL;
  }

  // 5. Configure child thread
  child->cr3 = child_cr3;
  child->is_forked_child = true;
  child->fork_ctx = child_regs;

  // 6. Copy file descriptors from parent to child (with reference counting)
  if (parent) {
    for (int i = 0; i < MAX_FDS; i++) {
      if (parent->fds[i]) {
        child->fds[i] = parent->fds[i];
        child->fd_offsets[i] = parent->fd_offsets[i];
        memcpy(child->fd_paths[i], parent->fd_paths[i],
               sizeof(child->fd_paths[i]));
        // Increment reference count for each inherited FD
        vfs_open(child->fds[i]);
      }
    }
    // 7. Clone memory state
    child->mm = kmalloc(sizeof(struct mm_struct));
    if (child->mm) {
      vma_list_init(&child->mm->vmas);
      vma_list_clone(&child->mm->vmas, &parent->mm->vmas);
      child->mm->brk_base = parent->mm->brk_base;
      child->mm->brk_current = parent->mm->brk_current;
      child->mm->mmap_next_addr = parent->mm->mmap_next_addr;
      child->mm->ref_count = 1;
      spinlock_init(&child->mm->lock);
    }
    memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
    child->cwd_node = parent->cwd_node;
    if (child->cwd_node)
      vfs_open(child->cwd_node);
    memcpy(child->signal_handlers, parent->signal_handlers,
           sizeof(child->signal_handlers));
    child->fs_base = parent->fs_base;
    child->umask = parent->umask;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->suid = parent->suid;
    child->sgid = parent->sgid;
    child->ctty = parent->ctty; // Inherit controlling terminal
  }

  klog_puts("[FORK] Child created with PID ");
  klog_uint64(child->tid);
  klog_puts("\n");

  // 8. Enqueue child thread now that it is fully configured
  sched_enqueue_thread(child, cpu_get_current());

  // 9. Return child PID to parent
  return child->tid;
}

// ── sys_clone ───────────────────────────────────────────────────────────────
static uint64_t sys_clone(struct syscall_regs *regs) {
  uint64_t flags = regs->rdi;
  uint64_t child_stack = regs->rsi;
  uint64_t ptid = regs->rdx;
  uint64_t ctid = regs->r10;
  uint64_t newtls = regs->r8;

  klog_puts("[CLONE] flags=");
  klog_uint64(flags);
  klog_puts(" stack=");
  klog_uint64(child_stack);
  klog_puts("\n");

  struct thread *parent = sched_get_current();
  if (!parent)
    return (uint64_t)-22; // EINVAL

  // 1. Determine address space strategy
  uint64_t child_cr3;
  struct mm_struct *child_mm = NULL;

  if (flags & CLONE_VM) {
    // Shared address space (threads)
    child_cr3 = parent->cr3;
    child_mm = parent->mm;
    // Shared state: Increment reference count
    if (child_mm) {
      spinlock_acquire(&child_mm->lock);
      child_mm->ref_count++;
      spinlock_release(&child_mm->lock);
    }
  } else {
    // Private (cloned) address space (process fork via clone)
    child_cr3 = vmm_clone_user_mappings_vma(
        (uint64_t *)parent->cr3, parent->mm ? &parent->mm->vmas : NULL);
    if (child_cr3 == 0) {
      return (uint64_t)-12; // ENOMEM
    }
    // Deep copy MM state
    child_mm = kmalloc(sizeof(struct mm_struct));
    if (child_mm) {
      vma_list_init(&child_mm->vmas);
      if (parent->mm) {
        vma_list_clone(&child_mm->vmas, &parent->mm->vmas);
        child_mm->brk_base = parent->mm->brk_base;
        child_mm->brk_current = parent->mm->brk_current;
        child_mm->mmap_next_addr = parent->mm->mmap_next_addr;
      }
      child_mm->ref_count = 1;
      spinlock_init(&child_mm->lock);
    }
  }

  // 2. Allocate and populate child registers
  struct syscall_regs *child_regs = kmalloc(sizeof(struct syscall_regs));
  if (!child_regs) {
    if (!(flags & CLONE_VM)) {
      // Ideally free child_cr3 here
    }
    return (uint64_t)-12;
  }
  memcpy(child_regs, regs, sizeof(struct syscall_regs));
  child_regs->rax = 0; // Child return value

  // If a new stack is provided, use it
  if (child_stack) {
    child_regs->rsp = child_stack;
  }

  // 3. Create kernel thread
  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, cpu_get_current(), false);
  if (!child) {
    kfree(child_regs);
    if (!(flags & CLONE_VM) && child_cr3) {
      vmm_free_user_pages_vma(child_cr3,
                              child_mm ? &child_mm->vmas : &parent->mm->vmas);
      if (child_mm) {
        vma_list_destroy(&child_mm->vmas);
        kfree(child_mm);
      }
    }
    return (uint64_t)-12;
  }

  // Clean up default MM
  if (child->mm && (flags & CLONE_VM)) {
    // If we're sharing VM, we definitely don't need the new private MM
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
    child->mm = NULL;
  } else if (child->mm && !(flags & CLONE_VM)) {
    // If we're cloning VM, we also replace it
    vma_list_destroy(&child->mm->vmas);
    kfree(child->mm);
    child->mm = NULL;
  }

  // 4. Configure child
  child->cr3 = child_cr3;
  child->is_forked_child = true;
  child->fork_ctx = child_regs;
  child->parent = parent;
  child->clone_flags = flags;

  // Handle TLS
  if (flags & CLONE_SETTLS) {
    child->fs_base = newtls;
  } else {
    child->fs_base = parent->fs_base;
  }

  // Handle TID placement
  if (flags & CLONE_PARENT_SETTID) {
    if (ptid && vmm_is_user_addr_range_valid(ptid, sizeof(uint32_t))) {
      *(uint32_t *)ptid = child->tid;
    }
  }
  if (flags & CLONE_CHILD_SETTID) {
    // We'd need to write this into the CHILD's memory space.
    // If CLONE_VM is set, we can do it now.
    if (flags & CLONE_VM) {
      if (ctid && vmm_is_user_addr_range_valid(ctid, sizeof(uint32_t))) {
        *(uint32_t *)ctid = child->tid;
      }
    }
    // Note: if not CLONE_VM, we should do it in fork_child_entry or similar.
  }

  // File descriptors
  if (flags & CLONE_FILES) {
    // Stub: we don't support true shared FD table yet, so just copy
    for (int i = 0; i < MAX_FDS; i++) {
      if (parent->fds[i]) {
        child->fds[i] = parent->fds[i];
        child->fd_offsets[i] = parent->fd_offsets[i];
        memcpy(child->fd_paths[i], parent->fd_paths[i],
               sizeof(child->fd_paths[i]));
        vfs_open(child->fds[i]);
      }
    }
  } else {
    for (int i = 0; i < MAX_FDS; i++) {
      if (parent->fds[i]) {
        child->fds[i] = parent->fds[i];
        child->fd_offsets[i] = parent->fd_offsets[i];
        memcpy(child->fd_paths[i], parent->fd_paths[i],
               sizeof(child->fd_paths[i]));
        vfs_open(child->fds[i]);
      }
    }
  }

  // Shared state copies
  child->mm = child_mm;
  memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
  child->cwd_node = parent->cwd_node;
  if (child->cwd_node)
    vfs_open(child->cwd_node);
  child->uid = parent->uid;
  child->gid = parent->gid;
  child->euid = parent->euid;
  child->egid = parent->egid;
  child->pgid = parent->pgid;
  memcpy(child->signal_handlers, parent->signal_handlers,
         sizeof(child->signal_handlers));

  // NOTE: sched_create_kernel_thread already added the child to the parent's
  // children list. Adding it again here would create a circular list and
  // cause wait4 to hang or double-reap.

  sched_enqueue_thread(child, cpu_get_current());

  return child->tid;
}

// ── sys_sysinfo ───────────────────────────────────────────────────────────
struct sysinfo {
  int64_t uptime;
  uint64_t loads[3];
  uint64_t totalram;
  uint64_t freeram;
  uint64_t sharedram;
  uint64_t bufferram;
  uint64_t totalswap;
  uint64_t freeswap;
  uint16_t procs;
  uint64_t totalhigh;
  uint64_t freehigh;
  uint32_t mem_unit;
  char _f[8]; // padding to 112 bytes (Linux ABI)
} __attribute__((packed));

static uint64_t sys_sysinfo(uint64_t info_ptr, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct sysinfo *info = (struct sysinfo *)info_ptr;
  if (!info)
    return (uint64_t)-14; // EFAULT

  info->uptime = (int64_t)(lapic_timer_get_ms() / 1000);
  info->loads[0] = 0;
  info->loads[1] = 0;
  info->loads[2] = 0;
  info->totalram = pmm_get_total_memory();
  info->freeram = (uint64_t)pmm_get_free_pages() * PAGE_SIZE;
  info->sharedram = 0;
  info->bufferram = 0;
  info->totalswap = 0;
  info->freeswap = 0;
  info->procs = sched_get_thread_count();
  info->totalhigh = 0;
  info->freehigh = 0;
  info->mem_unit = 1;
  return 0;
}

// ── sys_uname ─────────────────────────────────────────────────────────────
struct utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

static uint64_t sys_uname(uint64_t buf_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct utsname *buf = (struct utsname *)buf_ptr;
  if (!buf)
    return (uint64_t)-14; // EFAULT

  strcpy(buf->sysname, "AscentOS");
  strcpy(buf->nodename, "ascentos");
  strcpy(buf->release, "0.1.0-alpha");

  // Dynamic date/time from RTC
  char datetime[32];
  rtc_format_datetime(rtc_get_timestamp(), datetime, sizeof(datetime));

  char version[80];
  strcpy(version, "#1 SMP PREEMPT ");
  strcat(version, datetime);
  strcpy(buf->version, version);

  strcpy(buf->machine, "x86_64");
  strcpy(buf->domainname, "ascent-os.org");

  return 0;
}

// ── sys_uptime ─────────────────────────────────────────────────────────────
static uint64_t sys_uptime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return lapic_timer_get_ms();
}

static uint64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3,
                          uint64_t arg4, uint64_t arg5, uint64_t a5) {
  (void)option;
  (void)arg2;
  (void)arg3;
  (void)arg4;
  (void)arg5;
  (void)a5;
  return 0; // Success stub
}

// ── sys_umask ───────────────────────────────────────────────────────────────
static uint64_t sys_umask(struct syscall_regs *regs) {
  uint32_t mask = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;

  uint32_t old_mask = t->umask;
  t->umask = mask & 0777;
  return old_mask;
}

// ── sys_getuid ──────────────────────────────────────────────────────────────
static uint64_t sys_getuid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->uid;
}

// ── sys_getgid ──────────────────────────────────────────────────────────────
static uint64_t sys_getgid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->gid;
}

// ── sys_geteuid ─────────────────────────────────────────────────────────────
static uint64_t sys_geteuid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->euid;
}

// ── sys_getegid ─────────────────────────────────────────────────────────────
static uint64_t sys_getegid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->egid;
}

// ── sys_getresuid ───────────────────────────────────────────────────────────
static uint64_t sys_getresuid(struct syscall_regs *regs) {
  uint32_t *ruid_ptr = (uint32_t *)regs->rdi;
  uint32_t *euid_ptr = (uint32_t *)regs->rsi;
  uint32_t *suid_ptr = (uint32_t *)regs->rdx;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (ruid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)ruid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *ruid_ptr = t->uid;
  }
  if (euid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)euid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *euid_ptr = t->euid;
  }
  if (suid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)suid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *suid_ptr = t->suid;
  }

  return 0;
}

// ── sys_getresgid ───────────────────────────────────────────────────────────
static uint64_t sys_getresgid(struct syscall_regs *regs) {
  uint32_t *rgid_ptr = (uint32_t *)regs->rdi;
  uint32_t *egid_ptr = (uint32_t *)regs->rsi;
  uint32_t *sgid_ptr = (uint32_t *)regs->rdx;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (rgid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)rgid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *rgid_ptr = t->gid;
  }
  if (egid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)egid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *egid_ptr = t->egid;
  }
  if (sgid_ptr) {
    if (!vmm_is_user_addr_range_valid((uint64_t)sgid_ptr, sizeof(uint32_t)))
      return (uint64_t)-14;
    *sgid_ptr = t->sgid;
  }

  return 0;
}

// ── sys_setresuid ───────────────────────────────────────────────────────────
static uint64_t sys_setresuid(struct syscall_regs *regs) {
  (void)regs;
  // Stub: accept any uid changes silently (we're always root)
  return 0;
}

// ── sys_setresgid ───────────────────────────────────────────────────────────
static uint64_t sys_setresgid(struct syscall_regs *regs) {
  (void)regs;
  // Stub: accept any gid changes silently (we're always root)
  return 0;
}

// ── sys_setuid (syscall 105) ────────────────────────────────────────────────
static uint64_t sys_setuid(struct syscall_regs *regs) {
  uint32_t uid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  t->uid = uid;
  t->euid = uid;
  t->suid = uid;
  return 0;
}

// ── sys_setgid (syscall 106) ────────────────────────────────────────────────
static uint64_t sys_setgid(struct syscall_regs *regs) {
  uint32_t gid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  t->gid = gid;
  t->egid = gid;
  t->sgid = gid;
  return 0;
}

// ── sys_setfsuid (syscall 122) ──────────────────────────────────────────────
static uint64_t sys_setfsuid(struct syscall_regs *regs) {
  uint32_t fsuid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  // Return the previous fsuid value
  uint32_t old_fsuid = t->fsuid;
  t->fsuid = fsuid;
  return old_fsuid;
}

// ── sys_setfsgid (syscall 123) ──────────────────────────────────────────────
static uint64_t sys_setfsgid(struct syscall_regs *regs) {
  uint32_t fsgid = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;
  // Return the previous fsgid value
  uint32_t old_fsgid = t->fsgid;
  t->fsgid = fsgid;
  return old_fsgid;
}

// ── sys_getppid ─────────────────────────────────────────────────────────────
static uint64_t sys_getppid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  if (t->parent)
    return t->parent->tid;
  return 0;
}

// ── sys_setpgid ─────────────────────────────────────────────────────────────
static uint64_t sys_setpgid(struct syscall_regs *regs) {
  uint32_t pid = (uint32_t)regs->rdi;
  uint32_t pgid = (uint32_t)regs->rsi;
  struct thread *current = sched_get_current();
  struct thread *target = NULL;

  if (pid == 0) {
    target = current;
  } else {
    // Only allow setting PGID for self or children
    if (current->tid == pid) {
      target = current;
    } else {
      spinlock_acquire(&tid_lock);
      struct thread *c = current->children;
      while (c) {
        if (c->tid == pid) {
          target = c;
          break;
        }
        c = c->sibling_next;
      }
      spinlock_release(&tid_lock);
    }
  }

  if (!target)
    return (uint64_t)-3; // ESRCH

  if (pgid == 0)
    pgid = target->tid;
  target->pgid = pgid;
  return 0;
}

static uint64_t sys_getpgid(uint64_t pid, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (pid == 0)
    return current->pgid;
  struct thread *target = sched_get_thread_by_tid((uint32_t)pid);
  if (!target)
    return (uint64_t)-3; // ESRCH
  return target->pgid;
}

// ── sys_getpgrp ─────────────────────────────────────────────────────────────
static uint64_t sys_getpgrp(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->pgid;
}

// ── sys_setsid (stub) ──────────────────────────────────────────────────────
static uint64_t sys_setsid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->tid; // Return own PID as new session ID
}

// ── sys_setitimer (stub) ───────────────────────────────────────────────────
// X11 uses this but can work without real timer support
static uint64_t sys_setitimer(uint64_t which, uint64_t new_val_ptr,
                              uint64_t old_val_ptr, uint64_t _a3, uint64_t _a4,
                              uint64_t _a5) {
  (void)which;
  (void)new_val_ptr;
  (void)old_val_ptr;
  (void)_a3;
  (void)_a4;
  (void)_a5;
  // Stub: just return 0 success without actually setting a timer
  return 0;
}

// ── Resource Limits (getrlimit / prlimit64) ──────────────────────────────────
struct rlimit {
  uint64_t rlim_cur;
  uint64_t rlim_max;
};

#define RLIM_INFINITY ((uint64_t)-1)

static uint64_t sys_getrlimit(uint64_t resource, uint64_t rlim_ptr, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct rlimit *r = (struct rlimit *)rlim_ptr;
  if (!r)
    return (uint64_t)-14; // EFAULT

  // Simple stubs: everything is infinite for now
  r->rlim_cur = RLIM_INFINITY;
  r->rlim_max = RLIM_INFINITY;

  // Some apps specifically check RLIMIT_NOFILE
  if (resource == 7) { // RLIMIT_NOFILE
    r->rlim_cur = MAX_FDS;
    r->rlim_max = MAX_FDS;
  }

  return 0;
}

static uint64_t sys_prlimit64(struct syscall_regs *regs) {
  uint32_t pid = (uint32_t)regs->rdi;
  uint32_t resource = (uint32_t)regs->rsi;
  struct rlimit *new_limit = (struct rlimit *)regs->rdx;
  struct rlimit *old_limit = (struct rlimit *)regs->r10;

  if (pid != 0) {
    struct thread *t = sched_get_thread_by_tid(pid);
    if (!t)
      return (uint64_t)-3; // ESRCH
    if (t != sched_get_current())
      return (uint64_t)-1; // EPERM (only self for now)
  }

  if (old_limit) {
    old_limit->rlim_cur = RLIM_INFINITY;
    old_limit->rlim_max = RLIM_INFINITY;
    if (resource == 7) { // RLIMIT_NOFILE
      old_limit->rlim_cur = MAX_FDS;
      old_limit->rlim_max = MAX_FDS;
    }
  }

  if (new_limit) {
    // Stub: we don't actually enforce many limits yet,
    // so we just ignore the new limits for now.
  }

  return 0;
}

static uint64_t sys_membarrier(uint64_t cmd, uint64_t flags, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)cmd;
  (void)flags;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // Stub: success
  return 0;
}

// ── Registration ────────────────────────────────────────────────────────────
static uint64_t sys_sched_yield(uint64_t a0, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  sched_yield();
  return 0;
}

void syscall_register_process(void) {
  syscall_register(SYS_EXIT, sys_exit);
  syscall_register(SYS_EXIT_GROUP, sys_exit_group);
  syscall_register(SYS_SET_TID_ADDRESS, sys_set_tid_address);
  syscall_register(SYS_GETPID, sys_getpid);
  syscall_register(SYS_WAIT4, sys_wait4);
  syscall_register(SYS_UNAME, sys_uname);
  syscall_register(SYS_SYSINFO, sys_sysinfo);
  syscall_register(SYS_UPTIME, sys_uptime);
  syscall_register(SYS_GETCWD, sys_getcwd);
  syscall_register(SYS_CHDIR, sys_chdir);
  syscall_register(SYS_PRCTL, sys_prctl);
  syscall_register(SYS_SETITIMER, sys_setitimer);
  syscall_register_raw(SYS_FORK, sys_fork);
  syscall_register_raw(SYS_CLONE, sys_clone);
  syscall_register_raw(SYS_EXECVE, sys_execve);
  syscall_register_raw(SYS_UMASK, sys_umask);
  syscall_register_raw(SYS_GETUID, sys_getuid);
  syscall_register_raw(SYS_GETGID, sys_getgid);
  syscall_register_raw(SYS_GETEUID, sys_geteuid);
  syscall_register_raw(SYS_GETEGID, sys_getegid);
  syscall_register_raw(SYS_GETRESUID, sys_getresuid);
  syscall_register_raw(SYS_GETRESGID, sys_getresgid);
  syscall_register_raw(SYS_SETRESUID, sys_setresuid);
  syscall_register_raw(SYS_SETRESGID, sys_setresgid);
  syscall_register_raw(SYS_GETPPID, sys_getppid);
  syscall_register_raw(SYS_SETPGID, sys_setpgid);
  syscall_register_raw(SYS_GETPGRP, sys_getpgrp);
  syscall_register(SYS_GETPGID, sys_getpgid);
  syscall_register_raw(SYS_SETSID, sys_setsid);
  syscall_register_raw(SYS_SETUID, sys_setuid);
  syscall_register_raw(SYS_SETGID, sys_setgid);
  syscall_register_raw(SYS_SETFSUID, sys_setfsuid);
  syscall_register_raw(SYS_SETFSGID, sys_setfsgid);
  syscall_register(SYS_GETRLIMIT, sys_getrlimit);
  syscall_register_raw(SYS_PRLIMIT64, sys_prlimit64);
  syscall_register(SYS_MEMBARRIER, sys_membarrier);
  syscall_register(SYS_SCHED_YIELD, sys_sched_yield);
}
