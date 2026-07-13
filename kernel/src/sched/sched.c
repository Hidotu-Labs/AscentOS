#include "sched.h"
#include "../apic/lapic.h"
#include "../apic/lapic_timer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/idt.h"
#include "../cpu/msr.h"
#include "../fs/procfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"

// Futex waiter records are stack-resident and must be detached before a
// forced-exit thread stack is released.
void futex_remove_thread_waiters(struct thread *thread);

static void ipi_reschedule_handler(struct registers *regs) {
  (void)regs;
  /*
   * sched_yield() may switch away from this interrupt context indefinitely.
   * Acknowledging the IPI in the common ISR epilogue is therefore too late:
   * the LAPIC keeps the reschedule vector in-service and can withhold later
   * timer/IPI delivery from this CPU.  This is especially easy to trigger
   * when an exiting child wakes its parent on another CPU under KVM.
   *
   * The common epilogue will issue a second EOI if this context eventually
   * resumes; as with the LAPIC timer handler, that redundant EOI is harmless.
   */
  lapic_send_eoi();
  sched_yield();
}

static uint32_t next_tid = 1;
spinlock_t tid_lock = SPINLOCK_INIT;

static struct fd_table *fd_table_create(void) {
  struct fd_table *files = kmalloc(sizeof(struct fd_table));
  if (!files)
    return NULL;
  memset(files, 0, sizeof(struct fd_table));
  files->ref_count = 1;
  spinlock_init(&files->lock);
  return files;
}

static void thread_set_files(struct thread *t, struct fd_table *files) {
  t->files = files;
  t->fds = files ? files->fds : NULL;
  t->fd_offsets = files ? files->fd_offsets : NULL;
  t->fd_flags = files ? files->fd_flags : NULL;
  t->fd_paths = files ? files->fd_paths : NULL;
}

static void fd_path_put(struct fd_path *path) {
  if (path && __atomic_sub_fetch(&path->ref_count, 1, __ATOMIC_ACQ_REL) == 0)
    kfree(path);
}

bool fd_path_set(struct thread *t, int fd, const char *value) {
  if (!t || !t->files || fd < 0 || fd >= MAX_FDS)
    return false;
  struct fd_path *path = NULL;
  if (value && value[0]) {
    path = kmalloc(sizeof(*path));
    if (!path)
      return false;
    path->ref_count = 1;
    strncpy(path->value, value, sizeof(path->value) - 1);
    path->value[sizeof(path->value) - 1] = '\0';
  }
  struct fd_path *old = t->fd_paths[fd];
  t->fd_paths[fd] = path;
  fd_path_put(old);
  return true;
}

void fd_path_dup(struct thread *t, int dst, int src) {
  if (!t || dst < 0 || dst >= MAX_FDS || src < 0 || src >= MAX_FDS)
    return;
  struct fd_path *path = t->fd_paths[src];
  if (path)
    __atomic_add_fetch(&path->ref_count, 1, __ATOMIC_RELAXED);
  struct fd_path *old = t->fd_paths[dst];
  t->fd_paths[dst] = path;
  fd_path_put(old);
}

void fd_path_clear(struct thread *t, int fd) {
  if (!t || fd < 0 || fd >= MAX_FDS)
    return;
  struct fd_path *old = t->fd_paths[fd];
  t->fd_paths[fd] = NULL;
  fd_path_put(old);
}

const char *fd_path_value(struct thread *t, int fd) {
  if (!t || fd < 0 || fd >= MAX_FDS || !t->fd_paths[fd])
    return NULL;
  return t->fd_paths[fd]->value;
}

bool sched_ensure_files(struct thread *t) {
  if (!t)
    return false;
  if (t->files) {
    thread_set_files(t, t->files);
    return true;
  }

  struct fd_table *files = fd_table_create();
  if (!files)
    return false;
  thread_set_files(t, files);
  return true;
}

void sched_release_files(struct thread *t) {
  if (!t || !t->files)
    return;

  struct fd_table *files = t->files;
  bool last = false;
  if (t->is_forked_child) {
    klog_puts("[FDDBG] lock table ");
    klog_hex64((uint64_t)files);
    klog_puts(" refs=");
    klog_uint64(files->ref_count);
    klog_puts(" locked=");
    klog_uint64(files->lock.locked);
    klog_puts("\n");
  }
  spinlock_acquire(&files->lock);
  if (t->is_forked_child)
    klog_puts("[FDDBG] table locked\n");
  if (--files->ref_count == 0)
    last = true;
  spinlock_release(&files->lock);
  thread_set_files(t, NULL);

  if (!last)
    return;

  for (int i = 0; i < MAX_FDS; i++) {
    if (files->fds[i] && files->fds[i] != (vfs_node_t *)-1) {
      vfs_node_t *node = files->fds[i];
      if (t->is_forked_child) {
        klog_puts("[FDDBG] closing fd ");
        klog_uint64(i);
        klog_puts(" node=");
        klog_hex64((uint64_t)node);
        klog_puts(" refs=");
        klog_uint64(node->refcount);
        klog_puts("\n");
      }
      files->fds[i] = NULL;
      fd_path_put(files->fd_paths[i]);
      files->fd_paths[i] = NULL;
      vfs_close(node);
      if (t->is_forked_child)
        klog_puts("[FDDBG] close returned\n");
    }
  }
  kfree(files);
}

void sched_share_files(struct thread *child, struct thread *parent) {
  if (!child || !parent || !parent->files)
    return;

  sched_release_files(child);
  spinlock_acquire(&parent->files->lock);
  parent->files->ref_count++;

  spinlock_release(&parent->files->lock);
  thread_set_files(child, parent->files);
}

// Threads in a CLONE_THREAD group are not wait4() children. Queue them for
// destruction after they have switched off their kernel stacks.
static struct thread *reap_queue = NULL;
static spinlock_t reap_queue_lock = SPINLOCK_INIT;
static spinlock_t reap_worker_lock = SPINLOCK_INIT;

#define THREAD_STACK_SIZE 8192

static void *thread_stack_alloc(void) {
  void *phys = pmm_alloc_pages(THREAD_STACK_SIZE / PAGE_SIZE);
  if (!phys)
    return NULL;
  return (void *)((uint64_t)phys + pmm_get_hhdm_offset());
}

static void thread_stack_release(uint64_t stack_base) {
  if (!stack_base)
    return;
  void *phys = (void *)(stack_base - pmm_get_hhdm_offset());
  pmm_free_pages(phys, THREAD_STACK_SIZE / PAGE_SIZE);
}

static bool sched_thread_off_cpu(struct thread *t) {
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;
    if (__atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE) == t ||
        __atomic_load_n(&cpu->switching_from, __ATOMIC_ACQUIRE) == t)
      return false;
  }
  return true;
}

/* Timed waits are rare compared with scheduler entries. Keep insertion O(n)
 * and make the yield/timer hot path O(1) by maintaining the earliest deadline
 * at the head. All helpers require cpu->queue_lock. */
static void sched_deadline_remove_locked(struct cpu_info *cpu,
                                         struct thread *t) {
  if (!t->deadline_queued)
    return;

  struct thread **link = &cpu->deadline_head;
  while (*link && *link != t)
    link = &(*link)->deadline_next;
  if (*link == t)
    *link = t->deadline_next;

  t->deadline_next = NULL;
  t->deadline_queued = false;
}

static void sched_deadline_insert_locked(struct cpu_info *cpu,
                                         struct thread *t) {
  if (!t->wakeup_ticks)
    return;
  if (t->deadline_queued)
    sched_deadline_remove_locked(cpu, t);

  struct thread **link = &cpu->deadline_head;
  while (*link && (*link)->wakeup_ticks <= t->wakeup_ticks)
    link = &(*link)->deadline_next;
  t->deadline_next = *link;
  *link = t;
  t->deadline_queued = true;
}

static void sched_deadline_expire_locked(struct cpu_info *cpu, uint64_t now) {
  while (cpu->deadline_head && cpu->deadline_head->wakeup_ticks <= now) {
    struct thread *t = cpu->deadline_head;
    cpu->deadline_head = t->deadline_next;
    t->deadline_next = NULL;
    t->deadline_queued = false;

    if (t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED) {
      bool still_current = cpu->current_thread == t;
      t->state = still_current ? THREAD_RUNNING : THREAD_READY;
      t->wakeup_ticks = 0;
      cpu->runqueue_bitmap |= (1U << t->priority);
      if (!still_current)
        cpu->runnable_count++;
    } else {
      t->wakeup_ticks = 0;
    }
  }
}

static void sched_arm_next_deadline(struct cpu_info *cpu,
                                    struct thread *next_t) {
  uint64_t now = lapic_timer_get_ms();
  uint64_t deadline = 0;
  if (next_t && !next_t->is_idle) {
    cpu->quantum_deadline_ms = now + LAPIC_SCHED_QUANTUM_MS;
    deadline = cpu->quantum_deadline_ms;
  } else {
    cpu->quantum_deadline_ms = 0;
  }
  if (cpu->deadline_head &&
      (!deadline || cpu->deadline_head->wakeup_ticks < deadline))
    deadline = cpu->deadline_head->wakeup_ticks;
  if (deadline) lapic_timer_rearm_if_earlier(deadline);
}

// Global list of all threads (for wait4)
struct thread *global_thread_list = NULL;

extern void switch_context(struct thread *old_t, struct thread *new_t);
extern void thread_stub(void); // Defined in switch.asm

void sched_init(void) {
  // We expect this to be called after cpu_init() which populates the CPU list
  uint32_t count = cpu_get_count();

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    // Initialize AP scheduler state before cpu_init_aps() starts its timer.
    if (!cpu)
      continue;

    spinlock_init(&cpu->queue_lock);

    // Initialize all runqueues
    for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++) {
      cpu->runqueues[p] = NULL;
    }
    cpu->runqueue_bitmap = 0;
    cpu->deadline_head = NULL;

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

  bool kick_idle_cpu = target_cpu->current_thread == target_cpu->idle_thread;
  spinlock_release(&target_cpu->queue_lock);

  // A periodic tick used to notice newly runnable work. In one-shot mode an
  // idle CPU may have no timer armed, so explicitly force a scheduling event.
  if (kick_idle_cpu && lapic_is_ready()) {
    struct cpu_info *self = cpu_get_current();
    if (target_cpu == self)
      lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1);
    else
      lapic_send_ipi(target_cpu->apic_id, IPI_VECTOR_RESCHEDULE);
  }

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
  struct fd_table *files = fd_table_create();
  if (!files) {
    kfree(t);
    return NULL;
  }
  thread_set_files(t, files);
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
  t->fsuid = t->fsgid = 0;
  t->supplementary_group_count = 0;

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

  t->state = THREAD_READY;
  t->stack_size = THREAD_STACK_SIZE;

  t->stack_base = (uint64_t)thread_stack_alloc();

  if (!t->stack_base) {
    sched_release_files(t);
    if (t->cwd_node)
      vfs_close(t->cwd_node);
    if (t->mm) {
      vma_list_destroy(&t->mm->vmas);
      kfree(t->mm);
    }
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

  /* Publish only after all allocations and context initialization succeed. */
  spinlock_acquire(&tid_lock);
  t->tid = next_tid++;
  t->tgid = t->tid;
  t->ss_flags = SS_DISABLE;
  t->parent = current;
  if (current) {
    t->sibling_next = current->children;
    current->children = t;
    t->pgid = current->pgid;
    t->sid = current->sid;
  } else {
    t->pgid = t->tid;
    t->sid = t->tid;
  }
  t->global_next = global_thread_list;
  global_thread_list = t;
  spinlock_release(&tid_lock);

  // Balance and add to a CPU.s runqueue conditionally
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
          curr != __atomic_load_n(&richest_cpu->current_thread,
                                  __ATOMIC_ACQUIRE) &&
          curr != __atomic_load_n(&richest_cpu->switching_from,
                                  __ATOMIC_ACQUIRE) &&
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

  // Reap detached threads from a different scheduler context. Never reap the
  // current thread: it is still executing on the stack that will be freed.
  // Only one CPU performs destructive reclamation at a time; queue
  // publication remains concurrent.
  if (spinlock_try_acquire(&reap_worker_lock)) {
  while (1) {
    struct thread *victim = NULL;
    spinlock_acquire(&reap_queue_lock);
    struct thread **link = &reap_queue;
    while (*link) {
      if (*link != prev && sched_thread_off_cpu(*link)) {
        victim = *link;
        *link = victim->reap_next;
        victim->reap_next = NULL;
        break;
      }
      link = &(*link)->reap_next;
    }
    spinlock_release(&reap_queue_lock);

    if (!victim)
      break;
    sched_reap_thread(victim);
  }
    spinlock_release(&reap_worker_lock);
  }

  spinlock_acquire(&cpu->queue_lock);

  /* Publish a newly requested timeout once, then wake only deadlines that are
   * actually due. This replaces two full runqueue scans on every yield. */
  uint64_t now = lapic_timer_get_ticks();
  if ((prev->state == THREAD_SLEEPING || prev->state == THREAD_BLOCKED) &&
      prev->wakeup_ticks) {
    if (prev->wakeup_ticks <= now) {
      prev->state = THREAD_RUNNING;
      prev->wakeup_ticks = 0;
    } else {
      sched_deadline_insert_locked(cpu, prev);
    }
  }
  sched_deadline_expire_locked(cpu, now);

  // 1. If prev is ZOMBIE or DEAD, remove it from the runqueue entirely.
  //    BLOCKED/SLEEPING threads stay in the queue so sched_wakeup can
  //    find them in O(1) via cpu_index without re-enqueueing.
  if (prev->state == THREAD_ZOMBIE || prev->state == THREAD_DEAD) {
    sched_deadline_remove_locked(cpu, prev);
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

  sched_arm_next_deadline(cpu, next_t);

  if (next_t && next_t != prev) {
    if (prev->state == THREAD_RUNNING) {
      prev->state = THREAD_READY;
    }
    next_t->state = THREAD_RUNNING;
    // current_thread must describe the arriving task before interrupts can
    // run on it, but publishing it alone does not mean the old kernel stack
    // is unused yet. Keep a separate hazard pointer until switch_context has
    // actually loaded the new RSP. A remote reaper must check both fields.
    __atomic_store_n(&cpu->switching_from, prev, __ATOMIC_RELEASE);
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

    /* arch_prctl and clone keep these cached fields authoritative. Reading
     * both MSRs on every switch is redundant and especially costly in TCG. */
    if (prev->fs_base != next_t->fs_base)
      wrmsr(0xC0000100, next_t->fs_base);
    if (prev->gs_base != next_t->gs_base)
      wrmsr(0xC0000102, next_t->gs_base);

    spinlock_release(&cpu->queue_lock);
    switch_context(prev, next_t);
  } else {
    spinlock_release(&cpu->queue_lock);
  }

  // Only enable here if we are returning normally
  __asm__ volatile("sti");
}

void sched_queue_reap(struct thread *t) {
  if (!t || t->is_idle)
    return;

  spinlock_acquire(&reap_queue_lock);
  t->reap_next = reap_queue;
  reap_queue = t;
  spinlock_release(&reap_queue_lock);
}

void sched_tick(struct registers *regs) {
  (void)regs;
  struct cpu_info *cpu = cpu_get_current();
  struct thread *curr = cpu->current_thread;
  if (curr) {
    // Account elapsed runtime independently of interrupt frequency.
    uint64_t now = lapic_timer_get_ms();
    uint64_t elapsed = cpu->ticks ? now - cpu->ticks : 0;
    curr->runtime_total += elapsed;
    cpu->ticks = now;

    /* ITIMER_REAL is global and protected by tid_lock. Any CPU whose
     * one-shot deadline fires may deliver an expired alarm. */
    {
      spinlock_acquire(&tid_lock);
      for (struct thread *t = global_thread_list; t; t = t->global_next) {
        if (t->it_real_next && now >= t->it_real_next) {
          extern void signal_send(struct thread *, int);
          signal_send(t, SIGALRM);
          if (t->it_real_interval) {
            t->it_real_next = now + t->it_real_interval;
            lapic_timer_rearm_if_earlier(t->it_real_next);
          }
          else { t->it_real_next = 0; t->it_real_value = 0; }
        }
      }
      spinlock_release(&tid_lock);
    }

    sched_yield();
  }
}

struct thread *sched_get_current(void) {
  return cpu_get_current()->current_thread;
}

void sched_terminate_thread_group(struct thread *current) {
  if (!current)
    return;

  uint32_t killed = 0;
  spinlock_acquire(&tid_lock);
  for (struct thread *t = global_thread_list; t; t = t->global_next) {
    if (t == current || t->tgid != current->tgid || t->is_idle ||
        t->state == THREAD_DEAD || t->state == THREAD_ZOMBIE)
      continue;

    /* exit_group is process-wide. Mark siblings unschedulable before making
     * them visible to the asynchronous reaper. The reaper waits until a
     * running sibling is off-CPU before touching any of its resources. */
    t->reap_remove_runqueue = true;
    __atomic_store_n(&t->state, THREAD_DEAD, __ATOMIC_RELEASE);

    struct cpu_info *cpu = cpu_get_info(t->cpu_index);
    if (cpu && cpu->status != CPU_STATUS_OFFLINE &&
        __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE) == t)
      lapic_send_ipi(cpu->apic_id, IPI_VECTOR_RESCHEDULE);

    sched_queue_reap(t);
    killed++;
  }
  spinlock_release(&tid_lock);

  if (killed) {
    klog_puts("[EXIT_GROUP] queued sibling threads: ");
    klog_uint64(killed);
    klog_puts("\n");
  }
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

    if (t->cpu_index == cpu_local->cpu_id)
      sched_deadline_remove_locked(cpu_local, t);

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

      /* A remote exit_group changes the victim to DEAD before it gets here,
       * so its old state no longer tells us whether runnable_count included
       * it. Rebuild the small per-CPU summary after the rare removal. */
      cpu_local->runnable_count = 0;
      cpu_local->runqueue_bitmap = 0;
      for (int q = 0; q < SCHED_PRIORITY_LEVELS; q++) {
        struct thread *qhead = cpu_local->runqueues[q];
        if (!qhead)
          continue;
        struct thread *qcur = qhead;
        do {
          bool runnable = qcur->state == THREAD_READY ||
                          qcur->state == THREAD_RUNNING;
          /* sched_yield accounts the current task until it removes/switches
           * it, even after that task changes to BLOCKED, ZOMBIE, or DEAD. */
          if (runnable || cpu_local->current_thread == qcur)
            cpu_local->runnable_count++;
          if (runnable)
            cpu_local->runqueue_bitmap |= (1U << q);
          qcur = qcur->next;
        } while (qcur && qcur != qhead);
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

  // Linux process children belong to the thread group for wait purposes.
  // Keep them with a live group member when a pthread exits.
  struct thread *adopter = NULL;
  if (parent->tgid != parent->tid) {
    for (struct thread *candidate = global_thread_list; candidate;
         candidate = candidate->global_next) {
      if (candidate != parent && candidate->tgid == parent->tgid &&
          candidate->state != THREAD_DEAD &&
          candidate->state != THREAD_ZOMBIE) {
        adopter = candidate;
        if (candidate->tid == parent->tgid)
          break;
      }
    }
  }

  // Orphans from a terminating process go to the BSP idle task as init.
  if (!adopter) {
    adopter = global_thread_list;
    while (adopter && adopter->tid != 1)
      adopter = adopter->global_next;
  }

  struct thread *child = parent->children;
  while (child) {
    struct thread *next_sibling = child->sibling_next;
    child->parent = adopter;
    if (adopter) {
      child->sibling_next = adopter->children;
      adopter->children = child;
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

  futex_remove_thread_waiters(t);

  /* The reap-queue claimant already verified sched_thread_off_cpu() while
   * holding reap_queue_lock. DEAD threads cannot become runnable again, so
   * repeating that check here can only spin forever on a stale hazard. */

  /* Self-exiting tasks have already removed themselves. Siblings killed by
   * exit_group may have been blocked and therefore never run the scheduler's
   * self-removal path. This is safe after the off-CPU check and is a no-op if
   * the task is already absent. */
  if (t->reap_remove_runqueue)
    remove_from_runqueue(t);

  klog_puts("[REAP] Step 1: remove from lists\n");
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
  bool parent_is_live = false;
  if (t->parent) {
    for (struct thread *it = global_thread_list; it; it = it->global_next) {
      if (it == t->parent) {
        parent_is_live = true;
        break;
      }
    }
  }
  if (parent_is_live) {
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

  // exit_group can reap related clone threads in any order. Clear every
  // surviving back-pointer before the thread object is freed.
  for (struct thread *it = global_thread_list; it; it = it->global_next) {
    if (it->parent == t) {
      it->parent = NULL;
      it->sibling_next = NULL;
    }
  }
  t->parent = NULL;
  t->children = NULL;
  t->sibling_next = NULL;
  spinlock_release(&tid_lock);

  /* Monitoring tools poll /proc continuously. Drop the cached PID tree
   * after the task has been removed from global lookup. */
  procfs_release_pid_dir(t->tid);

  // 3. Free fork_ctx (saved register state)
  klog_puts("[REAP] Step 3: free fork_ctx\n");
  if (t->fork_ctx) {
    kfree(t->fork_ctx);
    t->fork_ctx = NULL;
  }

  // Normally released by process_do_exit(); retain this as a safety net for
  // kernel-thread and abnormal teardown paths.
  sched_release_files(t);
  if (t->cwd_node) {
    vfs_close(t->cwd_node);
    t->cwd_node = NULL;
  }

  // 4. Free user page tables (CR3) and MM if last thread
  if (t->mm) {
    /* Lifetime references are independent of VMA mutations. An atomic drop
     * avoids waiting on an mm lock from scheduler/reaper context and makes
     * exactly one reaper responsible for final destruction. */
    int refs = __atomic_sub_fetch(&t->mm->ref_count, 1, __ATOMIC_ACQ_REL);
    if (refs == 0) {
      klog_puts("[REAP] Last thread, freeing MM resources\n");
      if (t->cr3) {
        vmm_free_user_pages_vma(t->cr3, &t->mm->vmas);
        t->cr3 = 0;
      }
      vma_list_destroy(&t->mm->vmas);
      kfree(t->mm);
    } else {
      klog_puts("[REAP] MM still shared, skipping CR3 free\n");
      t->cr3 = 0; // Don't free for THIS thread
    }
    t->mm = NULL;
  }

  /* HHDM-backed stack pages need no page-table unmap during release. */
  thread_stack_release(t->stack_base);
  kfree(t);

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
  spinlock_release(&tid_lock);
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
      /* A wake may race between publishing BLOCKED and sched_yield(). The
       * current thread is still included in runnable_count in that window. */
      bool still_current =
          __atomic_load_n(&target->current_thread, __ATOMIC_ACQUIRE) == t;
      /* A wake that beats the caller into sched_yield() cancels the
       * block; the task is still executing and must never be published READY
       * for another CPU to steal. */
      sched_deadline_remove_locked(target, t);
      t->state = still_current ? THREAD_RUNNING : THREAD_READY;
      t->wakeup_ticks = 0;
      target->runqueue_bitmap |= (1 << t->priority);
      if (!still_current)
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
