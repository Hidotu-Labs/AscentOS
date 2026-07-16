#include "sched.h"
#include "hal/hal.h"
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
    klog_debug_puts("[FDDBG] lock table ");
    klog_debug_hex64((uint64_t)files);
    klog_debug_puts(" refs=");
    klog_debug_uint64(files->ref_count);
    klog_debug_puts(" locked=");
    klog_debug_uint64(files->lock.locked);
    klog_debug_puts("\n");
  }
  spinlock_acquire(&files->lock);
  if (t->is_forked_child)
    klog_debug_puts("[FDDBG] table locked\n");
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
        klog_debug_puts("[FDDBG] closing fd ");
        klog_debug_uint64(i);
        klog_debug_puts(" node=");
        klog_debug_hex64((uint64_t)node);
        klog_debug_puts(" refs=");
        klog_debug_uint64(node->refcount);
        klog_debug_puts("\n");
      }
      files->fds[i] = NULL;
      fd_path_put(files->fd_paths[i]);
      files->fd_paths[i] = NULL;
      vfs_close(node);
      if (t->is_forked_child)
        klog_debug_puts("[FDDBG] close returned\n");
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

static void sched_runqueue_append_locked(struct cpu_info *cpu,
                                         struct thread *t, uint8_t priority);
static bool sched_runqueue_remove_locked(struct cpu_info *cpu,
                                         struct thread *t);

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
      t->ready_since_ms = still_current ? 0 : now;
      if (!still_current) {
        t->priority = t->static_priority;
        sched_runqueue_append_locked(cpu, t, t->priority);
        cpu->runnable_count++;
      }
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


/* All run-queue helpers require cpu->queue_lock. */
static void sched_runqueue_append_locked(struct cpu_info *cpu,
                                         struct thread *t, uint8_t priority) {
  if (t->on_runqueue || priority >= SCHED_PRIORITY_LEVELS ||
      (t->state != THREAD_READY && t->state != THREAD_RUNNING)) return;
  struct thread *head = cpu->runqueues[priority];
  struct thread *tail = cpu->runqueue_tails[priority];
  if (!head) {
    cpu->runqueues[priority] = t;
    cpu->runqueue_tails[priority] = t;
    t->rq_next = t;
    t->rq_prev = t;
  } else {
    t->rq_prev = tail;
    t->rq_next = head;
    tail->rq_next = t;
    head->rq_prev = t;
    cpu->runqueue_tails[priority] = t;
  }
  t->on_runqueue = true;
  t->queued_priority = priority;
  t->priority = priority;
  if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
    cpu->runqueue_bitmap |= (1U << priority);
}

static bool sched_runqueue_remove_locked(struct cpu_info *cpu,
                                         struct thread *t) {
  if (!t->on_runqueue || t->cpu_index != cpu->cpu_id ||
      t->queued_priority >= SCHED_PRIORITY_LEVELS ||
      !t->rq_next || !t->rq_prev)
    return false;
  uint8_t p = t->queued_priority;
  if (cpu->runqueues[p] == t && t->rq_next == t) {
    cpu->runqueues[p] = NULL;
    cpu->runqueue_tails[p] = NULL;
    cpu->runqueue_bitmap &= ~(1U << p);
  } else {
    t->rq_prev->rq_next = t->rq_next;
    t->rq_next->rq_prev = t->rq_prev;
    if (cpu->runqueues[p] == t) cpu->runqueues[p] = t->rq_next;
    if (cpu->runqueue_tails[p] == t) cpu->runqueue_tails[p] = t->rq_prev;
  }
  t->rq_next = NULL;
  t->rq_prev = NULL;
  t->on_runqueue = false;
  t->queued_priority = SCHED_PRIORITY_LEVELS;
  return true;
}

static bool sched_requeue_priority_locked(struct cpu_info *cpu,
                                          struct thread *t,
                                          uint8_t new_priority) {
  if (t->priority == new_priority && t->on_runqueue) return true;
  if (!sched_runqueue_remove_locked(cpu, t)) return false;
  sched_runqueue_append_locked(cpu, t, new_priority);
  return true;
}

static void sched_age_runqueues_locked(struct cpu_info *cpu, uint64_t now) {
  if (now < cpu->next_aging_scan_ms) return;
  cpu->next_aging_scan_ms = now + SCHED_AGING_SCAN_INTERVAL_MS;
  for (uint8_t p = 1; p < SCHED_PRIORITY_LEVELS; p++) {
    struct thread *head = cpu->runqueues[p];
    if (!head) continue;
    struct thread *it = head;
    uint32_t count = 1;
    while ((it = it->rq_next) != head) count++;
    it = head;
    while (count--) {
      struct thread *next = it->rq_next;
      if (it->state == THREAD_READY && it->ready_since_ms && now > it->ready_since_ms) {
        uint64_t boost = (now - it->ready_since_ms) / SCHED_AGING_STEP_MS;
        uint8_t target = boost >= it->static_priority ? 0 :
                         (uint8_t)(it->static_priority - boost);
        if (target < it->priority) sched_requeue_priority_locked(cpu, it, target);
      }
      it = next;
    }
  }
}

static struct thread *sched_pick_next_locked(struct cpu_info *cpu) {
  while (cpu->runqueue_bitmap) {
    uint8_t p = (uint8_t)__builtin_ctz(cpu->runqueue_bitmap);
    struct thread *head = cpu->runqueues[p];
    if (!head) {
      cpu->runqueue_bitmap &= ~(1U << p);
      continue;
    }
    if (head->state != THREAD_READY && head->state != THREAD_RUNNING) {
      sched_runqueue_remove_locked(cpu, head);
      if (cpu->runnable_count) cpu->runnable_count--;
      continue;
    }
    cpu->runqueues[p] = head->rq_next;
    cpu->runqueue_tails[p] = head;
    return head;
  }
  return NULL;
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
      cpu->runqueue_tails[p] = NULL;
    }
    cpu->runqueue_bitmap = 0;
    cpu->deadline_head = NULL;
    cpu->next_aging_scan_ms = lapic_timer_get_ms() +
                              SCHED_AGING_SCAN_INTERVAL_MS;

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
  hal_irq_disable();
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

  hal_irq_disable();
  spinlock_acquire(&target_cpu->queue_lock);

  uint8_t p = t->priority;
  if (p >= SCHED_PRIORITY_LEVELS)
    p = SCHED_PRIORITY_DEFAULT;

  if (t->on_runqueue) {
    spinlock_release(&target_cpu->queue_lock);
    hal_irq_enable();
    return;
  }
  t->ready_since_ms = lapic_timer_get_ms();
  sched_runqueue_append_locked(target_cpu, t, p);
  if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
    target_cpu->runnable_count++;

  t->cpu_index = target_cpu->cpu_id;

  struct thread *running = target_cpu->current_thread;
  bool kick_cpu = running == target_cpu->idle_thread ||
                  (running && t->priority <= running->priority);
  spinlock_release(&target_cpu->queue_lock);

  /* One-shot mode has no periodic tick to notice newly runnable work. Ask
   * the target CPU to reschedule promptly instead of making an interactive
   * task wait for the running task's entire quantum. */
  if (kick_cpu && lapic_is_ready()) {
    struct cpu_info *self = cpu_get_current();
    if (target_cpu == self)
      lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1);
    else
      lapic_send_ipi(target_cpu->apic_id, IPI_VECTOR_RESCHEDULE);
  }

  hal_irq_enable();
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
  t->priority = SCHED_PRIORITY_DEFAULT;
  t->static_priority = SCHED_PRIORITY_DEFAULT;
  t->nice_value = 0;
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

  // Find a READY thread to steal. Removal is O(1) once selected.
  struct thread *stolen = NULL;
  for (int p = 0; p < SCHED_PRIORITY_LEVELS && !stolen; p++) {
    struct thread *head = richest_cpu->runqueues[p];
    if (!head) continue;
    struct thread *curr = head;
    do {
      struct thread *next = curr->rq_next;
      if (curr->state == THREAD_READY && !curr->is_idle &&
          curr != __atomic_load_n(&richest_cpu->current_thread,
                                  __ATOMIC_ACQUIRE) &&
          curr != __atomic_load_n(&richest_cpu->switching_from,
                                  __ATOMIC_ACQUIRE) &&
          (curr->cpu_affinity & (1ULL << cpu->cpu_id))) {
        stolen = curr;
        sched_runqueue_remove_locked(richest_cpu, curr);
        richest_cpu->runnable_count--;
        break;
      }
      curr = next;
    } while (curr != head);
  }

  spinlock_release(&richest_cpu->queue_lock);

  if (stolen) {
    // Add to local runqueue in O(1).
    stolen->cpu_index = cpu->cpu_id;
    sched_runqueue_append_locked(cpu, stolen, stolen->priority);
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
  hal_irq_disable();
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu->current_thread) {
    hal_irq_enable();
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
  sched_age_runqueues_locked(cpu, now);

  // Keep only runnable threads on run queues. Timed sleepers remain on the
  // deadline list and are re-enqueued when their timeout expires.
  if (prev->state == THREAD_ZOMBIE || prev->state == THREAD_DEAD) {
    sched_deadline_remove_locked(cpu, prev);
    sched_runqueue_remove_locked(cpu, prev);
    cpu->runnable_count--;
  } else if (prev->state == THREAD_BLOCKED || prev->state == THREAD_SLEEPING) {
    if (sched_runqueue_remove_locked(cpu, prev) && cpu->runnable_count)
      cpu->runnable_count--;
  }

  // 2. Select the highest effective-priority runnable thread.
  struct thread *next_t = sched_pick_next_locked(cpu);

  // 2.5 Load Balancing (Work Stealing)
  // If we found no READY thread on this CPU, try to steal one from others.
  if (!next_t) {
    sched_balance(cpu);
    next_t = sched_pick_next_locked(cpu);
  }

  // 3. Perform the switch
  if (!next_t) {
    next_t = cpu->idle_thread;
  }
  if (next_t && !next_t->is_idle) {
    next_t->ready_since_ms = 0;
    if (next_t->priority != next_t->static_priority)
      sched_requeue_priority_locked(cpu, next_t, next_t->static_priority);
  }

  sched_arm_next_deadline(cpu, next_t);

  if (next_t && next_t != prev) {
    if (prev->state == THREAD_RUNNING) {
      prev->state = THREAD_READY;
      prev->ready_since_ms = now;
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
  hal_irq_enable();
}

void sched_queue_reap(struct thread *t) {
  if (!t || t->is_idle)
    return;

  spinlock_acquire(&reap_queue_lock);
  t->reap_next = reap_queue;
  reap_queue = t;
  spinlock_release(&reap_queue_lock);
}

void sched_queue_reap_and_wait(struct thread *t) {
  if (!t || t->is_idle)
    return;

  sched_queue_reap(t);

  for (;;) {
    /* Give the exiting child a chance to switch off its kernel stack. */
    sched_yield();

    /* Holding the worker lock also waits for a concurrent reaper that may
     * already have removed and begun destroying this victim. */
    spinlock_acquire(&reap_worker_lock);
    spinlock_acquire(&reap_queue_lock);
    bool pending = false;
    for (struct thread *it = reap_queue; it; it = it->reap_next) {
      if (it == t) {
        pending = true;
        break;
      }
    }
    spinlock_release(&reap_queue_lock);
    spinlock_release(&reap_worker_lock);

    if (!pending)
      return;
  }
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

bool sched_validate_runqueues(struct cpu_info *cpu) {
  if (!cpu) return false;
  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&cpu->queue_lock);
  bool valid = true;
  uint32_t count = 0;
  uint32_t bitmap = 0;
  for (uint8_t p = 0; p < SCHED_PRIORITY_LEVELS && valid; p++) {
    struct thread *head = cpu->runqueues[p];
    struct thread *tail = cpu->runqueue_tails[p];
    if (!!head != !!tail) { valid = false; break; }
    if (!head) continue;
    if (head->rq_prev != tail || tail->rq_next != head) { valid = false; break; }
    struct thread *it = head;
    uint32_t guard = 0;
    do {
      if (!it->on_runqueue || it->queued_priority != p || it->priority != p ||
          (it->state != THREAD_READY && it->state != THREAD_RUNNING) ||
          !it->rq_next || !it->rq_prev || it->rq_next->rq_prev != it ||
          it->rq_prev->rq_next != it || it->cpu_index != cpu->cpu_id) {
        valid = false;
        break;
      }
      count++;
      bitmap |= (1U << p);
      it = it->rq_next;
    } while (it != head && ++guard < 100000);
    if (guard >= 100000) valid = false;
  }
  if (count != cpu->runnable_count || bitmap != cpu->runqueue_bitmap)
    valid = false;
  spinlock_release(&cpu->queue_lock);
  hal_irq_restore(flags);
  return valid;
}

bool sched_set_priority(struct thread *t, uint8_t priority, int8_t nice_value) {
  if (!t || t->is_idle || priority >= SCHED_PRIORITY_LEVELS) return false;
  struct cpu_info *cpu = cpu_get_info(t->cpu_index);
  if (!cpu || cpu->status == CPU_STATUS_OFFLINE) return false;
  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&cpu->queue_lock);
  uint8_t old_priority = t->priority;
  t->static_priority = priority;
  t->nice_value = nice_value;
  sched_requeue_priority_locked(cpu, t, priority);
  bool preempt = (t->state == THREAD_READY || t->state == THREAD_RUNNING) &&
                 priority < old_priority;
  spinlock_release(&cpu->queue_lock);
  if (preempt && lapic_is_ready()) {
    struct cpu_info *self = cpu_get_current();
    if (cpu == self) lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1);
    else lapic_send_ipi(cpu->apic_id, IPI_VECTOR_RESCHEDULE);
  }
  hal_irq_restore(flags);
  return true;
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
    klog_debug_puts("[EXIT_GROUP] queued sibling threads: ");
    klog_debug_uint64(killed);
    klog_debug_puts("\n");
  }
}

void sched_print_tasks(void) {
  console_puts("TID  CPU  PRIO  STATE       RSP\n");
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    hal_irq_disable();
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

          curr = curr->rq_next;
        } while (curr != first);
      }
    }
    spinlock_release(&cpu->queue_lock);
    hal_irq_enable();
  }
}

bool sched_terminate_thread(uint32_t tid) {
  struct thread *t = sched_get_thread_by_tid(tid);
  if (!t || t->is_idle || t->state == THREAD_DEAD ||
      t->state == THREAD_ZOMBIE)
    return false;
  __atomic_store_n(&t->state, THREAD_DEAD, __ATOMIC_RELEASE);
  struct cpu_info *cpu = cpu_get_info(t->cpu_index);
  if (cpu && cpu->status != CPU_STATUS_OFFLINE &&
      __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE) == t)
    lapic_send_ipi(cpu->apic_id, IPI_VECTOR_RESCHEDULE);
  sched_queue_reap(t);
  return true;
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

// Helper: remove thread from its CPU's runqueue.
static void remove_from_runqueue(struct thread *t) {
  if (!t) return;
  struct cpu_info *cpu_local = cpu_get_info(t->cpu_index);
  if (!cpu_local || cpu_local->status == CPU_STATUS_OFFLINE) return;
  spinlock_acquire(&cpu_local->queue_lock);
  sched_deadline_remove_locked(cpu_local, t);
  if (sched_runqueue_remove_locked(cpu_local, t)) {
    cpu_local->runnable_count = 0;
    cpu_local->runqueue_bitmap = 0;
    for (int q = 0; q < SCHED_PRIORITY_LEVELS; q++) {
      struct thread *head = cpu_local->runqueues[q];
      if (!head) continue;
      struct thread *it = head;
      do {
        bool runnable = it->state == THREAD_READY || it->state == THREAD_RUNNING;
        if (runnable || cpu_local->current_thread == it)
          cpu_local->runnable_count++;
        if (runnable) cpu_local->runqueue_bitmap |= (1U << q);
        it = it->rq_next;
      } while (it != head);
    }
  }
  spinlock_release(&cpu_local->queue_lock);
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

  klog_debug_puts("[REAP] Reaping thread ");
  klog_debug_uint64(t->tid);
  klog_debug_puts("\n");

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

  klog_debug_puts("[REAP] Step 1: remove from lists\n");
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
  klog_debug_puts("[REAP] Step 3: free fork_ctx\n");
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
      klog_debug_puts("[REAP] Last thread, freeing MM resources\n");
      if (t->cr3) {
        vmm_free_user_pages_vma(t->cr3, &t->mm->vmas);
        t->cr3 = 0;
      }
      vma_list_destroy(&t->mm->vmas);
      kfree(t->mm);
    } else {
      klog_debug_puts("[REAP] MM still shared, skipping CR3 free\n");
      t->cr3 = 0; // Don't free for THIS thread
    }
    t->mm = NULL;
  }

  /* HHDM-backed stack pages need no page-table unmap during release. */
  thread_stack_release(t->stack_base);
  kfree(t);

  klog_debug_puts("[REAP] Done\n");
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

  hal_irq_state_t rflags = hal_irq_save();

  // O(1) wakeup: use the saved CPU assignment and append a descheduled
  // blocked thread to its base-priority queue.
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
      if (!still_current) {
        t->priority = t->static_priority;
        t->ready_since_ms = lapic_timer_get_ms();
        sched_runqueue_append_locked(target, t, t->priority);
        target->runnable_count++;
      } else {
        t->ready_since_ms = 0;
      }
    }
    spinlock_release(&target->queue_lock);

    /* A same-CPU wake previously waited as long as a complete quantum. This
     * is particularly visible in Xorg/client request-response workloads. */
    struct cpu_info *self = cpu_get_current();
    if (target->apic_id != self->apic_id) {
      lapic_send_ipi(target->apic_id, IPI_VECTOR_RESCHEDULE);
    } else if (self->current_thread &&
               t->priority <= self->current_thread->priority) {
      lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1);
    }
  } else {
    // Fallback: thread has no valid cpu_index (freshly created?)
    t->state = THREAD_READY;
    t->wakeup_ticks = 0;
    sched_enqueue_thread(t, cpu_get_current());
  }

  hal_irq_restore(rflags);
}
