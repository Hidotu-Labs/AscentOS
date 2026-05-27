#ifndef SCHED_H
#define SCHED_H

#include "../cpu/isr.h"
#include "../fs/vfs.h"
#include "../mm/vma.h"
#include <stddef.h>
#include <stdint.h>

// Forward declaration for embedded wait queue entry
struct wait_queue_entry;
typedef struct wait_queue_entry wait_queue_entry_t;

#include "../lock/spinlock.h"

// Shared memory management structure for 1:1 threads
struct mm_struct {
  struct vma_list vmas;    // Virtual memory areas
  uint64_t brk_base;       // Base of the heap
  uint64_t brk_current;    // Current end of the heap
  uint64_t mmap_next_addr; // Bump-pointer for anonymous mmap
  int ref_count;           // Reference count for sharing across threads
  spinlock_t lock;         // Lock for thread-safe MM state updates
};

#define MAX_FDS 256

// Signal constants
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30
#define SIGSYS 31

#define SIG_DFL 0
#define SIG_IGN 1

struct k_sigaction {
  void (*sa_handler)(int);
  uint64_t sa_flags;
  void (*sa_restorer)(void);
  uint64_t sa_mask;
};

typedef enum {
  THREAD_RUNNING,
  THREAD_READY,
  THREAD_BLOCKED,
  THREAD_SLEEPING,
  THREAD_DEAD,
  THREAD_ZOMBIE
} thread_state_t;

#define SCHED_PRIORITY_LEVELS 32
#define SCHED_PRIORITY_INTERACTIVE 0
#define SCHED_PRIORITY_DEFAULT 16
#define SCHED_PRIORITY_BACKGROUND 24
#define SCHED_PRIORITY_IDLE 31

// Information saved on context switch.
// We push callee-saved registers manually in switch.asm.
struct context {
  uint64_t r15;
  uint64_t r14;
  uint64_t r13;
  uint64_t r12;
  uint64_t rbx;
  uint64_t rbp;
  uint64_t ret_addr; // RIP (pushed automatically by call)
} __attribute__((packed));

struct thread {
  uint64_t rsp; // Must be first field (offset 0) for optimal assembly
  uint32_t tid;
  uint8_t fpu_state[512] __attribute__((aligned(16))); // Saved SSE/FPU state
  uint64_t stack_base;
  uint64_t stack_size;
  thread_state_t state;
  uint64_t wakeup_ticks;
  vfs_node_t *fds[MAX_FDS];
  uint64_t fd_offsets[MAX_FDS]; // Track seek offset per file descriptor
  char fd_paths[MAX_FDS][256];  // Track full path for each file descriptor
  uint64_t cr3;                 // Per-process page table (0 = inherited/kernel)
  bool is_forked_child;         // True for forked children (affects sys_exit)
  bool is_idle;                 // True for idle thread (cannot be terminated)
  void *fork_ctx;               // Saved register state for child entry
  bool is_main_session;    // True if this is the primary user session (Bash)
  uint64_t clone_flags;    // Flags passed to clone()
  struct thread *parent;   // Pointer to parent thread (for wait4)
  struct thread *children; // Head of children list
  struct thread *sibling_next; // Link to next sibling in parent's children list
  uint32_t pgid;               // Process group ID
  int exit_status;             // Status code when exiting (for wait4)
  uint64_t *tid_address;       // Pointer to user-space TID for set_tid_address
  struct thread *global_next;  // Used to link all threads together
  struct thread *next;         // Used for runqueue / blocked queue
  char cwd_path[256];          // Current working directory
  vfs_node_t *cwd_node;        // Current working directory VFS node
  struct mm_struct *mm;        // Shared memory management state
  uint64_t fs_base;            // User FS_BASE (TLS) — inherited across fork
  uint32_t uid;                // User ID
  uint32_t gid;                // Group ID
  uint32_t euid;               // Effective User ID
  uint32_t egid;               // Effective Group ID
  uint32_t suid;               // Saved set-user-ID
  uint32_t sgid;               // Saved set-group-ID
  uint32_t fsuid;              // File system User ID
  uint32_t fsgid;              // File system Group ID
  uint32_t umask;              // File creation mask

  // Controlling terminal (set when opening PTY slave as session leader)
  vfs_node_t *ctty; // Controlling terminal (PTY slave or console)

  // Signal state
  struct k_sigaction signal_handlers[64];
  uint64_t pending_signals;
  uint64_t signal_mask;

  // Embedded wait queue entry for safe blocking across context switches
  // Using stack-allocated entries is unsafe because the stack frame becomes
  // invalid when the thread is descheduled, leading to corrupted wait queues
  struct wait_queue_entry *wq_entry_next; // For multi-wait (epoll)

  // Scheduling/MLFQ priority state
  uint32_t cpu_index;      // Index of CPU this thread is enqueued on
  uint8_t priority;        // Current dynamic priority (0-31)
  uint8_t static_priority; // Base priority
  uint64_t time_slice;     // Remaining ticks in current quantum
  uint64_t runtime_total;  // Total CPU time consumed
  uint64_t runtime_burst;  // CPU time used in current quantum (for MLFQ)
};

void sched_init(void);
void sched_run_phase1_test(void);
void sched_wakeup(struct thread *t);
void sched_yield(void);

struct cpu_info;
struct thread *sched_create_kernel_thread(void (*entry_point)(void),
                                          struct cpu_info *explicit_cpu,
                                          bool enqueue);

void sched_tick(struct registers *regs);
void sched_yield(void);

// Returns the current thread *for the CPU currently executing this code*
struct thread *sched_get_current(void);

// Load balancing / dispatching
void sched_enqueue_thread(struct thread *t, struct cpu_info *explicit_cpu);

// Task management for shell
void sched_print_tasks(void);
bool sched_terminate_thread(uint32_t tid);
struct thread *sched_get_thread_by_tid(uint32_t tid);

// Reap a zombie thread (remove from runqueue, free resources)
void sched_reap_thread(struct thread *t);

// Reparent children to init
void sched_reparent_children(struct thread *parent);

int alloc_fd(struct thread *t);

// Userspace Management
#include "elf.h"
#include <stdbool.h>
bool elf_load(const char *path, uint64_t *pml4, elf_info_t *out_info);
// Linux-style stack: path string near top, then auxv/envp/argv/argc; RSP points
// at argc.
uint64_t process_build_initial_stack(uint64_t stack_top, const char *path,
                                     const char **argv, const char **envp,
                                     const elf_info_t *elf_info);
bool process_exec_argv(const char **argv);
void process_do_exit(uint64_t status);

#define ASCENTOS_USER_STACK_TOP 0x00007FFFF0000000ULL

#endif