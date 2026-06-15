#include "sched.h"
#include "../apic/lapic.h"
#include "../apic/lapic_timer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/idt.h"
#include "../cpu/msr.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"

static void ipi_reschedule_handler(struct registers *regs) {
  (void)regs;
  // EOI is handled by isr_handler
  sched_yield();
}

static uint32_t next_tid = 1;
spinlock_t tid_lock = SPINLOCK_INIT;

// Deferred reaping structures
struct dead_thread_info {
  uint64_t stack_base;
  uint64_t thread_ptr;
  struct dead_thread_info *next;
};
struct dead_thread_info *dead_threads = NULL;
spinlock_t dead_threads_lock = SPINLOCK_INIT;

// Global list of all threads (for wait4)
struct thread *global_thread_list = NULL;

extern void switch_context(struct thread *old_t, struct thread *new_t);
extern void thread_stub(void); // Defined in switch.asm

void sched_init(void) {
  // We expect this to be called after cpu_init() which populates the CPU list
  uint32_t count = cpu_get_count();

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    // Initialize all runqueues
    for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++) {
      cpu->runqueues[p] = NULL;
    }
    cpu->runqueue_bitmap = 0;

    // Register reschedule IPI handler once on BSP
    if (i == 0) {
      register_interrupt_handler(IPI_VECTOR_RESCHEDULE, ipi_reschedule_handler);
    }

    // Create the idle thread for this specific CPU
    struct thread *idle_thread = kmalloc(sizeof(struct thread));
    memset(idle_thread, 0, sizeof(struct thread));
    idle_thread->cwd_path[0] = '/';
    idle_thread->cwd_node = fs_root;
    if (fs_root)
      vfs_open(fs_root);
    // Assign a proper TID to the idle thread (don't use 0)
    spinlock_acquire(&tid_lock);
    idle_thread->tid = next_tid++;
    spinlock_release(&tid_lock);
    idle_thread->tgid =
        idle_thread->tid;               // Each process is its own group leader
    idle_thread->ss_flags = SS_DISABLE; // No alternate signal stack by default
    idle_thread->is_idle = true;
    idle_thread->pgid = idle_thread->tid;
    idle_thread->state = THREAD_RUNNING;

    // Idle thread always at lowest priority
    idle_thread->priority = SCHED_PRIORITY_IDLE;
    idle_thread->static_priority = SCHED_PRIORITY_IDLE;
    idle_thread->time_slice = 100; // Large slice for idle
    idle_thread->runtime_total = 0;
    idle_thread->runtime_burst = 0;
    strcpy(idle_thread->comm, "idle");

    // Idle threads don't really use user MM, but give them a stub to avoid NULL
    // derefs
    idle_thread->mm = kmalloc(sizeof(struct mm_struct));
    if (idle_thread->mm) {
      memset(idle_thread->mm, 0, sizeof(struct mm_struct));
      vma_list_init(&idle_thread->mm->vmas);
      idle_thread->mm->ref_count = 1;
      spinlock_init(&idle_thread->mm->lock);
    }

    idle_thread->stack_size = CPU_STACK_SIZE;
    idle_thread->stack_base = cpu->stack_top - CPU_STACK_SIZE;

    // Idle threads have no parent
    idle_thread->parent = NULL;

    spinlock_acquire(&tid_lock);
    idle_thread->global_next = global_thread_list;
    global_thread_list = idle_thread;
    // Do NOT enqueue the idle thread; it's handled specially by sched_yield
    cpu->idle_thread = idle_thread;
    cpu->current_thread = idle_thread;
    idle_thread->cpu_affinity = (1ULL << count) - 1;
    if (count == 64)
      idle_thread->cpu_affinity = ~0ULL;
    spinlock_release(&tid_lock);

    spinlock_release(&cpu->queue_lock);
  }
}

static void thread_exit(void) {
  // Current thread finished execution. Mark dead and yield.
  __asm__ volatile("cli");
  struct cpu_info *cpu = cpu_get_current();
  if (cpu->current_thread) {
    cpu->current_thread->state = THREAD_DEAD;
  }
  // Infinite loop, yield will switch away
  while (1) {
    sched_yield();
  }
}

#define THREAD_STACK_SIZE 8192

void sched_enqueue_thread(struct thread *t, struct cpu_info *explicit_cpu) {
  struct cpu_info *target_cpu = explicit_cpu;

  if (!target_cpu) {
    // Intelligent Load Balancing: Pick the CPU with the fewest threads
    uint32_t min_threads = 0xFFFFFFFF;
    uint32_t count = cpu_get_count();

    for (uint32_t i = 0; i < count; i++) {
      struct cpu_info *cpu = cpu_get_info(i);
      if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
        continue;

      // Skip if this CPU is not in the thread's affinity mask
      if (!(t->cpu_affinity & (1ULL << i)))
        continue;

      // We don't necessarily need the lock here for a heuristic,
      // but it's safer.
      if (cpu->runnable_count < min_threads) {
        min_threads = cpu->runnable_count;
        target_cpu = cpu;
      }
    }
  }

  // Fallback just in case
  if (!target_cpu || target_cpu->status == CPU_STATUS_OFFLINE) {
    target_cpu = cpu_get_bsp();
  }

  __asm__ volatile("cli");
  spinlock_acquire(&target_cpu->queue_lock);

  uint8_t p = t->priority;
  if (p >= SCHED_PRIORITY_LEVELS)
    p = SCHED_PRIORITY_DEFAULT;

  if (!target_cpu->runqueues[p]) {
    target_cpu->runqueues[p] = t;
    t->next = t;
    target_cpu->runqueue_bitmap |= (1 << p);
  } else {
    // Insert at tail of circular list
    struct thread *head = target_cpu->runqueues[p];
    struct thread *tail = head;
    while (tail->next != head) {
      tail = tail->next;
    }
    tail->next = t;
    t->next = head;
  }

  if (t->state == THREAD_READY || t->state == THREAD_RUNNING) {
    target_cpu->runnable_count++;
  }

  t->cpu_index = target_cpu->cpu_id;

  spinlock_release(&target_cpu->queue_lock);
  __asm__ volatile("sti");
}

struct thread *sched_create_kernel_thread(void (*entry)(void),
                                          struct cpu_info *explicit_cpu,
                                          bool enqueue) {
  // Allocate thread struct
  struct thread *t = kmalloc(sizeof(struct thread));
  if (!t)
    return NULL;

  memset(t, 0, sizeof(struct thread));
  // Default comm for kernel threads; overwritten by execve for user processes
  strcpy(t->comm, "kthread");
  t->cwd_path[0] = '/';
  t->cwd_node = fs_root;
  struct thread *current = sched_get_current();
  if (current) {
    strcpy(t->cwd_path, current->cwd_path);
    t->cwd_node = current->cwd_node;
  }
  if (t->cwd_node)
    vfs_open(t->cwd_node);
  t->umask = 0022;
  t->uid = t->gid = t->euid = t->egid = t->suid = t->sgid = 0;

  // Root kernel threads get their own MM by default.
  // fork/clone/exec will replace/refcount this later as needed.
  t->mm = kmalloc(sizeof(struct mm_struct));
  if (t->mm) {
    memset(t->mm, 0, sizeof(struct mm_struct));
    vma_list_init(&t->mm->vmas);
    t->mm->ref_count = 1;
    spinlock_init(&t->mm->lock);
  }

  uint32_t cpu_count = cpu_get_count();
  t->cpu_affinity = (1ULL << cpu_count) - 1;
  if (cpu_count == 64)
    t->cpu_affinity = ~0ULL;

  spinlock_acquire(&tid_lock);
  t->tid = next_tid++;
  t->tgid = t->tid;         // Default: each thread is its own group leader
  t->ss_flags = SS_DISABLE; // No alternate signal stack by default
  t->global_next = global_thread_list;
  global_thread_list = t;

  // Set parent and link into hierarchy
  t->parent = current;
  if (current) {
    t->sibling_next = current->children;
    current->children = t;
    t->pgid = current->pgid; // Inherit PGID by default
    t->sid = current->sid;   // Inherit SID by default
  } else {
    t->pgid = t->tid; // Root threads have PGID = TID
    t->sid = t->tid;  // Root threads have SID = TID
  }
  spinlock_release(&tid_lock);

  t->state = THREAD_READY;
  t->stack_size = THREAD_STACK_SIZE;

  t->stack_base = (uint64_t)kmalloc(THREAD_STACK_SIZE);

  if (!t->stack_base) {
    kfree(t);
    return NULL;
  }

  uint64_t stack_top = t->stack_base + THREAD_STACK_SIZE;
  stack_top &= ~0xFULL; // Align stack

  // 1. Push the thread exit function (simulating a return address)
  stack_top -= 8;
  *(uint64_t *)stack_top = (uint64_t)thread_exit;

  // 2. Setup context frame
  stack_top -= sizeof(struct context);
  struct context *ctx = (struct context *)stack_top;
  memset(ctx, 0, sizeof(struct context));

  // Store entry function in r12 which thread_stub will call
  ctx->r12 = (uint64_t)entry;

  // `switch_context` does `ret`, popping this address
  ctx->ret_addr = (uint64_t)thread_stub;

  t->rsp = stack_top;

  // Initialize FPU state
  memset(t->fpu_state, 0, 512);

  // We can't easily call fninit here for the child buffer without clobbering
  // current FPU state. However, we can just let switch_context handle it
  // if we ensure it's zeroed (most CPUs treat zero as okay) or use a static
  // init.
  static uint8_t fpu_init_done = 0;
  static uint8_t initial_fpu_state[512] __attribute__((aligned(16)));
  if (!fpu_init_done) {
    __asm__ volatile("fninit; fxsave64 %0" : "=m"(initial_fpu_state));
    fpu_init_done = 1;
  }
  memcpy(t->fpu_state, initial_fpu_state, 512);

  // Balance and add to a CPU's runqueue conditionally
  if (enqueue) {
    sched_enqueue_thread(t, explicit_cpu);
  }

  return t;
}

static void sched_balance(struct cpu_info *cpu) {
  uint32_t count = cpu_get_count();
  if (count <= 1)
    return;

  // Find the CPU with the most threads.
  struct cpu_info *richest_cpu = NULL;
  uint32_t max_threads = 0;

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *other = cpu_get_info(i);
    if (!other || other == cpu || other->status == CPU_STATUS_OFFLINE)
      continue;

    if (other->runnable_count > max_threads) {
      max_threads = other->runnable_count;
      richest_cpu = other;
    }
  }

  // Only steal if the richest CPU actually has surplus threads (more than 1).
  // Stealing the only thread might cause unnecessary migration or thrashing.
  if (!richest_cpu || max_threads <= 1)
    return;

  // Try to acquire the remote lock without blocking to avoid deadlocks.
  if (!spinlock_try_acquire(&richest_cpu->queue_lock))
    return;

  // Find a thread to steal.
  struct thread *stolen = NULL;
  for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++) {
    struct thread *head = richest_cpu->runqueues[p];
    if (!head)
      continue;

    struct thread *curr = head;
    struct thread *prev = NULL;
    do {
      // Find a READY thread (don't steal the currently running one).
      // Ensure the thread is allowed to run on THIS CPU (the stealing CPU).
      if (curr->state == THREAD_READY && !curr->is_idle &&
          (curr->cpu_affinity & (1ULL << cpu->cpu_id))) {
        stolen = curr;

        // Remove from remote runqueue
        if (curr->next == curr) {
          richest_cpu->runqueues[p] = NULL;
          richest_cpu->runqueue_bitmap &= ~(1 << p);
        } else {
          if (!prev) {
            // Find predecessor in circular list
            prev = head;
            while (prev->next != curr)
              prev = prev->next;
          }
          prev->next = curr->next;
          if (richest_cpu->runqueues[p] == curr) {
            richest_cpu->runqueues[p] = curr->next;
          }
        }
        richest_cpu->runnable_count--;
        break;
      }
      prev = curr;
      curr = curr->next;
    } while (curr != head);

    if (stolen)
      break;
  }

  spinlock_release(&richest_cpu->queue_lock);

  if (stolen) {
    // Add to local runqueue
    stolen->cpu_index = cpu->cpu_id;
    uint8_t p = stolen->priority;
    if (!cpu->runqueues[p]) {
      cpu->runqueues[p] = stolen;
      stolen->next = stolen;
      cpu->runqueue_bitmap |= (1 << p);
    } else {
      struct thread *head = cpu->runqueues[p];
      struct thread *tail = head;
      while (tail->next != head)
        tail = tail->next;
      tail->next = stolen;
      stolen->next = head;
    }
    cpu->runnable_count++;

    klog_puts("[SCHED] CPU ");
    klog_uint64(cpu->cpu_id);
    klog_puts(" stole thread ");
    klog_uint64(stolen->tid);
    klog_puts(" from CPU ");
    klog_uint64(richest_cpu->cpu_id);
    klog_puts("\n");
  }
}

void sched_yield(void) {
  __asm__ volatile("cli");
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu->current_thread) {
    __asm__ volatile("sti");
    return;
  }

  struct thread *prev = cpu->current_thread;

  spinlock_acquire(&cpu->queue_lock);

  // Wake ALL expired sleeping threads FIRST.
  // This ensures that if a high-priority task wakes up, we can switch to it
  // immediately.
  {
    uint64_t now = lapic_timer_get_ticks();
    // Only scan current CPU's queues to avoid cross-core pointer corruption or
    // complex locking. Threads in AscentOS are currently sticky to their CPU.
    for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++) {
      struct thread *head = cpu->runqueues[p];
      if (!head)
        continue;
      struct thread *curr = head;
      do {
        if (!curr)
          break; // Defensive
        if ((curr->state == THREAD_SLEEPING || curr->state == THREAD_BLOCKED) &&
            curr->wakeup_ticks != 0 && now >= curr->wakeup_ticks) {
          curr->state = THREAD_READY;
          curr->wakeup_ticks = 0;
          cpu->runqueue_bitmap |= (1 << p);
          cpu->runnable_count++;
        }
        curr = curr->next;
      } while (curr != head && curr != NULL);
    }
  }

  // 1. If prev is ZOMBIE or DEAD, remove it from the runqueue entirely.
  //    BLOCKED/SLEEPING threads stay in the queue so sched_wakeup can
  //    find them in O(1) via cpu_index without re-enqueueing.
  if (prev->state == THREAD_ZOMBIE || prev->state == THREAD_DEAD) {
    uint8_t p = prev->priority;
    if (cpu->runqueues[p]) {
      if (prev->next == prev) {
        cpu->runqueues[p] = NULL;
        cpu->runqueue_bitmap &= ~(1 << p);
      } else {
        struct thread *pred = prev;
        while (pred->next != prev)
          pred = pred->next;
        pred->next = prev->next;
        if (cpu->runqueues[p] == prev)
          cpu->runqueues[p] = prev->next;
      }
    }
    cpu->runnable_count--;
  } else if (prev->state == THREAD_BLOCKED || prev->state == THREAD_SLEEPING) {
    // Thread just transitioned out of a runnable state
    cpu->runnable_count--;
  }

  // 2. Select the next thread to run.
  //    Scan each priority level's circular list for a READY/RUNNING thread.
  //    Skip BLOCKED/SLEEPING threads (they remain in the queue for fast
  //    wakeup). If no runnable thread is found at a level, clear the bitmap
  //    bit.
  struct thread *next_t = NULL;
  uint32_t bitmap_copy = cpu->runqueue_bitmap;

  while (bitmap_copy != 0) {
    int p = __builtin_ctz(bitmap_copy);
    struct thread *head = cpu->runqueues[p];
    if (!head) {
      cpu->runqueue_bitmap &= ~(1 << p);
      bitmap_copy &= ~(1 << p);
      continue;
    }

    struct thread *curr = head;
    bool found = false;
    do {
      if (curr->state == THREAD_READY || curr->state == THREAD_RUNNING) {
        next_t = curr;
        // Advance head past selected thread for round-robin
        cpu->runqueues[p] = curr->next;
        found = true;
        break;
      }
      curr = curr->next;
    } while (curr != head);

    if (found)
      break;

    // No runnable thread at this priority — clear the bitmap bit
    cpu->runqueue_bitmap &= ~(1 << p);
    bitmap_copy &= ~(1 << p);
  }

  // 2.5 Load Balancing (Work Stealing)
  // If we found no READY thread on this CPU, try to steal one from others.
  if (!next_t) {
    sched_balance(cpu);
    // Try to select again if we stole something.
    if (cpu->runqueue_bitmap != 0) {
      bitmap_copy = cpu->runqueue_bitmap;
      while (bitmap_copy != 0) {
        int p = __builtin_ctz(bitmap_copy);
        struct thread *head = cpu->runqueues[p];
        if (head) {
          struct thread *curr = head;
          do {
            if (curr->state == THREAD_READY) {
              next_t = curr;
              cpu->runqueues[p] = curr->next;
              break;
            }
            curr = curr->next;
          } while (curr != head);
        }
        if (next_t)
          break;
        bitmap_copy &= ~(1 << p);
      }
    }
  }

  // 3. Perform the switch
  if (!next_t) {
    next_t = cpu->idle_thread;
  }

  if (next_t && next_t != prev) {
    if (prev->state == THREAD_RUNNING) {
      prev->state = THREAD_READY;
    }
    next_t->state = THREAD_RUNNING;
    cpu->current_thread = next_t;

    cpu->stack_top = (next_t->stack_base + next_t->stack_size) & ~0xFULL;
    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(cpu->stack_top);

    uint64_t target_cr3 = next_t->cr3 ? next_t->cr3 : cpu->kernel_cr3;
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (target_cr3 != current_cr3) {
      __asm__ volatile("mov %0, %%cr3" ::"r"(target_cr3) : "memory");
    }

    // Save/Restore TLS MSRs
    prev->fs_base = rdmsr(0xC0000100);
    wrmsr(0xC0000100, next_t->fs_base);

    spinlock_release(&cpu->queue_lock);
    switch_context(prev, next_t);
  } else {
    spinlock_release(&cpu->queue_lock);
  }

  // Only enable here if we are returning normally
  __asm__ volatile("sti");
}

void sched_tick(struct registers *regs) {
  (void)regs;
  struct cpu_info *cpu = cpu_get_current();
  struct thread *curr = cpu->current_thread;
  if (curr) {
    // Account one tick (1 ms at LAPIC_TIMER_HZ=1000) of CPU time
    curr->runtime_total++;

    // ITIMER_REAL handling
    if (curr->it_real_value > 0) {
      if (curr->it_real_value <= 1) {
        // Timer expired!
        extern void signal_send(struct thread * t, int sig);
        signal_send(curr, SIGALRM);

        // Reload if requested
        if (curr->it_real_interval > 0) {
          curr->it_real_value = curr->it_real_interval;
        } else {
          curr->it_real_value = 0;
        }
      } else {
        curr->it_real_value--;
      }
    }

    sched_yield();
  }
}

struct thread *sched_get_current(void) {
  return cpu_get_current()->current_thread;
}

void sched_print_tasks(void) {
  console_puts("TID  CPU  PRIO  STATE       RSP\n");
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    __asm__ volatile("cli");
    spinlock_acquire(&cpu->queue_lock);

    for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++) {
      struct thread *first = cpu->runqueues[p];
      struct thread *curr = first;
      if (curr) {
        do {
          // TID
          char tid_buf[10];
          int j = 0;
          uint32_t tid = curr->tid;
          if (tid == 0) {
            tid_buf[j++] = '0';
          } else {
            while (tid > 0) {
              tid_buf[j++] = '0' + (tid % 10);
              tid /= 10;
            }
          }
          while (j < 4)
            tid_buf[j++] = ' ';
          for (int k = j - 1; k >= 0; k--)
            console_putchar(tid_buf[k]);
          console_putchar(' ');

          // CPU
          console_putchar('0' + (cpu->cpu_id % 10));
          console_puts("    ");

          // PRIO
          char prio_buf[4];
          prio_buf[0] = '0' + (curr->priority / 10);
          prio_buf[1] = '0' + (curr->priority % 10);
          prio_buf[2] = ' ';
          prio_buf[3] = '\0';
          console_puts(prio_buf);
          console_puts("  ");

          // STATE
          switch (curr->state) {
          case THREAD_RUNNING:
            console_puts("RUNNING   ");
            break;
          case THREAD_READY:
            console_puts("READY     ");
            break;
          case THREAD_BLOCKED:
            console_puts("BLOCKED   ");
            break;
          case THREAD_SLEEPING:
            console_puts("SLEEPING  ");
            break;
          case THREAD_DEAD:
            console_puts("DEAD      ");
            break;
          case THREAD_ZOMBIE:
            console_puts("ZOMBIE    ");
            break;
          }

          // RSP (Hex)
          console_puts("0x");
          uint64_t rsp = curr->rsp;
          for (int bit = 60; bit >= 0; bit -= 4) {
            int nibble = (rsp >> bit) & 0xF;
            if (nibble < 10)
              console_putchar('0' + nibble);
            else
              console_putchar('A' + (nibble - 10));
          }
          console_putchar('\n');

          curr = curr->next;
        } while (curr != first);
      }
    }
    spinlock_release(&cpu->queue_lock);
    __asm__ volatile("sti");
  }
}

bool sched_terminate_thread(uint32_t tid) {
  // Cannot kill idle threads
  // (We check by TID since we'll search for the thread by TID first)

  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    __asm__ volatile("cli");
    spinlock_acquire(&cpu->queue_lock);
    for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++) {
      struct thread *first = cpu->runqueues[p];
      struct thread *curr = first;
      if (curr) {
        do {
          if (curr->tid == tid && !curr->is_idle) {
            curr->state = THREAD_DEAD;
            spinlock_release(&cpu->queue_lock);
            __asm__ volatile("sti");
            return true;
          }
          curr = curr->next;
        } while (curr != first);
      }
    }
    spinlock_release(&cpu->queue_lock);
    __asm__ volatile("sti");
  }
  return false;
}

// Helper: remove thread from global thread list
static void remove_from_global_list(struct thread *t) {
  if (!global_thread_list)
    return;

  spinlock_acquire(&tid_lock);

  if (global_thread_list == t) {
    global_thread_list = t->global_next;
  } else {
    struct thread *prev = global_thread_list;
    while (prev && prev->global_next != t) {
      prev = prev->global_next;
    }
    if (prev) {
      prev->global_next = t->global_next;
    }
  }

  spinlock_release(&tid_lock);
}

// Helper: remove thread from its CPU's runqueue
static void remove_from_runqueue(struct thread *t) {
  // Find which CPU has this thread
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu_local = cpu_get_info(i);
    if (!cpu_local || cpu_local->status == CPU_STATUS_OFFLINE)
      continue;

    __asm__ volatile("cli");
    spinlock_acquire(&cpu_local->queue_lock);

    // Check all priority levels
    for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++) {
      struct thread *head = cpu_local->runqueues[p];
      if (!head)
        continue;

      struct thread *curr = head;
      struct thread *prev_node = NULL;
      bool found = false;

      // Find t and its predecessor
      do {
        if (curr == t) {
          found = true;
          break;
        }
        prev_node = curr;
        curr = curr->next;
      } while (curr != head);

      if (!found)
        continue;

      // Found it. prev_node is now the predecessor.
      if (t->state == THREAD_READY || t->state == THREAD_RUNNING) {
        cpu_local->runnable_count--;
      }

      if (t->next == t) {
        // Only thread in this priority queue
        cpu_local->runqueues[p] = NULL;
        cpu_local->runqueue_bitmap &= ~(1 << p);
      } else {
        // More than one thread. We need to find the predecessor if we haven't
        // already.
        if (!prev_node) {
          prev_node = head;
          while (prev_node->next != t)
            prev_node = prev_node->next;
        }
        prev_node->next = t->next;
        if (cpu_local->runqueues[p] == t) {
          cpu_local->runqueues[p] = t->next;
        }
      }

      spinlock_release(&cpu_local->queue_lock);
      __asm__ volatile("sti");
      return;
    }

    spinlock_release(&cpu_local->queue_lock);
    __asm__ volatile("sti");
  }
}

void sched_reparent_children(struct thread *parent) {
  if (!parent)
    return;

  spinlock_acquire(&tid_lock);
  struct thread *child = parent->children;
  while (child) {
    struct thread *next_sibling = child->sibling_next;

    // Reparent to BSP idle thread (TID 1) as a fallback for init
    // In a mature kernel, this would be the actual 'init' process.
    struct thread *init = global_thread_list;
    while (init && init->tid != 1) {
      init = init->global_next;
    }

    child->parent = init;
    if (init) {
      child->sibling_next = init->children;
      init->children = child;
    } else {
      child->sibling_next = NULL;
    }

    child = next_sibling;
  }
  parent->children = NULL;
  spinlock_release(&tid_lock);
}

void sched_reap_thread(struct thread *t) {
  if (!t || t->is_idle)
    return;

  klog_puts("[REAP] Reaping thread ");
  klog_uint64(t->tid);
  klog_puts("\n");

  // 1. Remove from lists (global, parent hierarchy, runqueue)
  // We do runqueue first as it uses CPU locks, then global/parent using
  // tid_lock.
  klog_puts("[REAP] Step 1: remove from runqueue\n");
  remove_from_runqueue(t);

  // 1.25 Ensure the thread is not currently active on any CPU (race prevention)
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu_local = cpu_get_info(i);
    while (cpu_local->current_thread == t) {
      __asm__ volatile("pause");
    }
  }

  klog_puts("[REAP] Step 2: remove from lists\n");
  spinlock_acquire(&tid_lock);

  // 1.5 Remove from global thread list
  if (global_thread_list == t) {
    global_thread_list = t->global_next;
  } else {
    struct thread *prev_g = global_thread_list;
    while (prev_g && prev_g->global_next != t)
      prev_g = prev_g->global_next;
    if (prev_g)
      prev_g->global_next = t->global_next;
  }

  // 1.75 Remove from parent's children list
  if (t->parent) {
    if (t->parent->children == t) {
      t->parent->children = t->sibling_next;
    } else {
      struct thread *p = t->parent->children;
      while (p && p->sibling_next != t)
        p = p->sibling_next;
      if (p)
        p->sibling_next = t->sibling_next;
    }
  }
  spinlock_release(&tid_lock);

  // 3. Free fork_ctx (saved register state)
  klog_puts("[REAP] Step 3: free fork_ctx\n");
  if (t->fork_ctx) {
    kfree(t->fork_ctx);
    t->fork_ctx = NULL;
  }

  // 4. Free user page tables (CR3) and MM if last thread
  if (t->mm) {
    spinlock_acquire(&t->mm->lock);
    t->mm->ref_count--;
    if (t->mm->ref_count == 0) {
      spinlock_release(&t->mm->lock);
      klog_puts("[REAP] Last thread, freeing MM resources\n");
      if (t->cr3) {
        vmm_free_user_pages_vma(t->cr3, &t->mm->vmas);
        t->cr3 = 0;
      }
      vma_list_destroy(&t->mm->vmas);
      kfree(t->mm);
    } else {
      spinlock_release(&t->mm->lock);
      klog_puts("[REAP] MM still shared, skipping CR3 free\n");
      t->cr3 = 0; // Don't free for THIS thread
    }
    t->mm = NULL;
  }

  // Deferred Free: Free any previously deferred dead threads.
  // By the time a new thread is being reaped, any previously dead threads
  // have long been switched away from, guaranteeing their stacks are safe to
  // free.
  spinlock_acquire(&dead_threads_lock);
  struct dead_thread_info *curr_dead = dead_threads;
  dead_threads = NULL;
  spinlock_release(&dead_threads_lock);

  while (curr_dead) {
    if (curr_dead->stack_base) {
      kfree((void *)curr_dead->stack_base);
    }
    if (curr_dead->thread_ptr) {
      kfree((void *)curr_dead->thread_ptr);
    }
    struct dead_thread_info *to_free = curr_dead;
    curr_dead = curr_dead->next;
    kfree(to_free);
  }

  // Defer Freeing for THIS thread
  struct dead_thread_info *dead_info = kmalloc(sizeof(struct dead_thread_info));
  if (dead_info) {
    dead_info->stack_base = t->stack_base;
    dead_info->thread_ptr = (uint64_t)t;
    spinlock_acquire(&dead_threads_lock);
    dead_info->next = dead_threads;
    dead_threads = dead_info;
    spinlock_release(&dead_threads_lock);
    t->stack_base = 0;
  }

  klog_puts("[REAP] Done\n");
}

struct thread *sched_get_thread_by_tid(uint32_t tid) {
  spinlock_acquire(&tid_lock);
  struct thread *curr = global_thread_list;
  while (curr) {
    if (curr->tid == tid) {
      spinlock_release(&tid_lock);
      return curr;
    }
    curr = curr->global_next;
  }
  return NULL;
}

uint16_t sched_get_thread_count(void) {
  spinlock_acquire(&tid_lock);
  uint16_t count = 0;
  struct thread *curr = global_thread_list;
  while (curr) {
    count++;
    curr = curr->global_next;
  }
  spinlock_release(&tid_lock);
  return count;
}

struct thread *sched_get_thread_list_head(void) { return global_thread_list; }

void sched_wakeup(struct thread *t) {
  if (!t)
    return;
  if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
    return; // Already runnable, nothing to do

  uint64_t rflags;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) : : "memory");

  // O(1) wakeup: blocked threads stay in their CPU's runqueue,
  // so we just use the stored cpu_index to flip state + bitmap.
  struct cpu_info *target = cpu_get_info(t->cpu_index);
  if (target && target->status != CPU_STATUS_OFFLINE) {
    spinlock_acquire(&target->queue_lock);
    if (t->state != THREAD_READY && t->state != THREAD_RUNNING) {
      t->state = THREAD_READY;
      t->wakeup_ticks = 0;
      target->runqueue_bitmap |= (1 << t->priority);
      target->runnable_count++;
    }
    spinlock_release(&target->queue_lock);

    // If the woken thread is on a different CPU, send an IPI
    struct cpu_info *self = cpu_get_current();
    if (target->apic_id != self->apic_id) {
      lapic_send_ipi(target->apic_id, IPI_VECTOR_RESCHEDULE);
    }
  } else {
    // Fallback: thread has no valid cpu_index (freshly created?)
    t->state = THREAD_READY;
    t->wakeup_ticks = 0;
    sched_enqueue_thread(t, cpu_get_current());
  }

  __asm__ volatile("push %0; popfq" : : "r"(rflags) : "memory");
}
