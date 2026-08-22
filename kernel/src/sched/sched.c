#include "sched.h"
#include "hal/hal.h"
#include "../apic/lapic.h"
#include "../apic/lapic_timer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/features.h"
#include "../cpu/fpu.h"
#include "../cpu/idt.h"
#include "../cpu/msr.h"
#include "../fs/procfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pcid.h"
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
        eevfd_place_entity(&cpu->eevfd, &t->se, false);
        eevfd_enqueue_entity(&cpu->eevfd, &t->se);
        t->on_runqueue = true;
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
    /* Ceiling division: round up to avoid firing late */
    uint64_t slice_ms = (next_t->se.slice_ns + 999999ULL) / 1000000ULL;
    if (slice_ms < 1) slice_ms = 1;
    cpu->quantum_deadline_ms = now + slice_ms;
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
    if (!cpu)
      continue;

    spinlock_init(&cpu->queue_lock);
    eevfd_rq_init(&cpu->eevfd);
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
    idle_thread->tgid = idle_thread->tid;
    idle_thread->ss_flags = SS_DISABLE;
    idle_thread->is_idle = true;
    idle_thread->pgid = idle_thread->tid;
    idle_thread->state = THREAD_RUNNING;

    // Idle thread initialized with nice 19 (lowest priority)
    eevfd_entity_init(&idle_thread->se, 19, EEVFD_MAX_SLICE_NS);
    idle_thread->priority = SCHED_PRIORITY_IDLE;
    idle_thread->static_priority = SCHED_PRIORITY_IDLE;
    idle_thread->nice_value = 19;
    idle_thread->time_slice = 100;
    idle_thread->runtime_total = 0;
    idle_thread->runtime_burst = 0;
    strcpy(idle_thread->comm, "idle");

    idle_thread->mm = kmalloc(sizeof(struct mm_struct));
    if (idle_thread->mm) {
      memset(idle_thread->mm, 0, sizeof(struct mm_struct));
      vma_list_init(&idle_thread->mm->vmas);
      idle_thread->mm->ref_count = 1;
      spinlock_init(&idle_thread->mm->lock);
    }

    idle_thread->stack_size = CPU_STACK_SIZE;
    idle_thread->stack_base = cpu->stack_top - CPU_STACK_SIZE;
    idle_thread->parent = NULL;

    spinlock_acquire(&tid_lock);
    idle_thread->global_next = global_thread_list;
    global_thread_list = idle_thread;
    cpu->idle_thread = idle_thread;
    cpu->current_thread = idle_thread;
    idle_thread->cpu_affinity = (1ULL << count) - 1;
    if (count == 64)
      idle_thread->cpu_affinity = ~0ULL;
    spinlock_release(&tid_lock);
  }
}

static void thread_exit(void) {
  hal_irq_disable();
  struct cpu_info *cpu = cpu_get_current();
  if (cpu->current_thread) {
    cpu->current_thread->state = THREAD_DEAD;
  }
  while (1) {
    sched_yield();
  }
}

__attribute__((optimize("O3"))) void sched_enqueue_thread(struct thread *t, struct cpu_info *explicit_cpu) {
  struct cpu_info *target_cpu = explicit_cpu;

  if (!target_cpu) {
    uint32_t min_threads = 0xFFFFFFFF;
    uint32_t count = cpu_get_count();

    for (uint32_t i = 0; i < count; i++) {
      struct cpu_info *cpu = cpu_get_info(i);
      if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
        continue;

      if (!(t->cpu_affinity & (1ULL << i)))
        continue;

      if (cpu->runnable_count < min_threads) {
        min_threads = cpu->runnable_count;
        target_cpu = cpu;
      }
    }
  }

  if (!target_cpu || target_cpu->status == CPU_STATUS_OFFLINE) {
    target_cpu = cpu_get_bsp();
  }

  hal_irq_disable();
  spinlock_acquire(&target_cpu->queue_lock);

  if (t->on_runqueue || t->se.on_rq) {
    spinlock_release(&target_cpu->queue_lock);
    hal_irq_enable();
    return;
  }

  t->ready_since_ms = lapic_timer_get_ms();
  eevfd_place_entity(&target_cpu->eevfd, &t->se, (t->se.vruntime == 0));
  eevfd_enqueue_entity(&target_cpu->eevfd, &t->se);
  t->on_runqueue = true;
  if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
    target_cpu->runnable_count++;

  t->cpu_index = target_cpu->cpu_id;

  struct thread *running = target_cpu->current_thread;
  bool kick_cpu = !running || running == target_cpu->idle_thread || running->is_idle ||
                  eevfd_check_preempt(&target_cpu->eevfd, &running->se, &t->se);
  spinlock_release(&target_cpu->queue_lock);

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
    t->mm->pcid = pcid_alloc();
    spinlock_init(&t->mm->lock);
  }


  uint32_t cpu_count = cpu_get_count();
  t->cpu_affinity = (1ULL << cpu_count) - 1;
  if (cpu_count == 64)
    t->cpu_affinity = ~0ULL;

  t->state = THREAD_READY;
  eevfd_entity_init(&t->se, 0, EEVFD_BASE_SLICE_NS);
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

__attribute__((optimize("O3"))) static void sched_balance(struct cpu_info *cpu) {
  uint32_t count = cpu_get_count();
  if (count <= 1)
    return;

  // Find the CPU with highest load
  struct cpu_info *richest_cpu = NULL;
  uint32_t max_threads = 0;

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *other = cpu_get_info(i);
    if (!other || other == cpu || other->status == CPU_STATUS_OFFLINE)
      continue;

    uint32_t nr = other->eevfd.nr_running;
    if (nr > max_threads) {
      max_threads = nr;
      richest_cpu = other;
    }
  }

  // Imbalance threshold: only steal if richest has 2+ more tasks than us
  if (!richest_cpu || max_threads < 2 ||
      (max_threads - cpu->eevfd.nr_running) < 2)
    return;

  if (!spinlock_try_acquire(&richest_cpu->queue_lock))
    return;

  // Steal from RIGHTMOST (highest vruntime = most indebted task).
  // This minimizes disruption to the source CPU's fairness.
  struct thread *stolen = NULL;
  struct rb_node *n = rb_last(&richest_cpu->eevfd.tasks_tree);
  while (n) {
    struct sched_entity *se = rb_entry(n, struct sched_entity, rb_node);
    struct thread *curr = rb_entry(se, struct thread, se);
    if (curr->state == THREAD_READY && !curr->is_idle &&
        curr != __atomic_load_n(&richest_cpu->current_thread, __ATOMIC_ACQUIRE) &&
        curr != __atomic_load_n(&richest_cpu->switching_from, __ATOMIC_ACQUIRE) &&
        (curr->cpu_affinity & (1ULL << cpu->cpu_id))) {
      stolen = curr;
      eevfd_dequeue_entity(&richest_cpu->eevfd, &stolen->se);
      stolen->on_runqueue = false;
      if (richest_cpu->runnable_count)
        richest_cpu->runnable_count--;
      break;
    }
    n = rb_prev(n);
  }

  if (stolen) {
    stolen->cpu_index = cpu->cpu_id;
    eevfd_migrate_entity(&richest_cpu->eevfd, &cpu->eevfd, &stolen->se);
    eevfd_enqueue_entity(&cpu->eevfd, &stolen->se);
    stolen->on_runqueue = true;
    cpu->runnable_count++;
  }

  spinlock_release(&richest_cpu->queue_lock);
}

__attribute__((optimize("O3"))) static void sched_schedule(bool voluntary_yield) {
  hal_irq_disable();
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu->current_thread) {
    hal_irq_enable();
    return;
  }

  struct thread *prev = cpu->current_thread;

  // Reap detached threads from a different scheduler context.
  if (__atomic_load_n(&reap_queue, __ATOMIC_RELAXED) != NULL &&
      spinlock_try_acquire(&reap_worker_lock)) {
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

  uint64_t now = lapic_timer_get_ms();
  uint64_t now_ns = lapic_timer_get_ns();

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

  /* Fast-path: single runnable task on this core. Zero tree overhead.
   * Only applicable on non-voluntary preemption/ticks. */
  if (!voluntary_yield && prev->state == THREAD_RUNNING && !prev->is_idle && cpu->eevfd.nr_running == 0) {
    eevfd_update_curr(&cpu->eevfd, now_ns);
    prev->runtime_total = prev->se.prev_sum_exec_ns / 1000000ULL;
    sched_arm_next_deadline(cpu, prev);
    spinlock_release(&cpu->queue_lock);
    hal_irq_enable();
    return;
  }

  if (!prev->is_idle) {
    eevfd_update_curr(&cpu->eevfd, now_ns);
    prev->runtime_total = prev->se.prev_sum_exec_ns / 1000000ULL;
    eevfd_put_prev_entity(&cpu->eevfd, &prev->se);
  }

  if (prev->state == THREAD_ZOMBIE || prev->state == THREAD_DEAD) {
    sched_deadline_remove_locked(cpu, prev);
    if (prev->se.on_rq) {
      eevfd_dequeue_entity(&cpu->eevfd, &prev->se);
      prev->on_runqueue = false;
    }
    if (cpu->runnable_count)
      cpu->runnable_count--;
  } else if (prev->state == THREAD_BLOCKED || prev->state == THREAD_SLEEPING) {
    if (prev->se.on_rq) {
      eevfd_dequeue_entity(&cpu->eevfd, &prev->se);
      prev->on_runqueue = false;
    }
    if (cpu->runnable_count)
      cpu->runnable_count--;
  } else {
    /* prev is still running or ready */
    if (!prev->is_idle && !prev->se.on_rq) {
      if (voluntary_yield) {
        /* Linux yield_task_fair: advance vruntime so other waiting tasks run next */
        if (cpu->eevfd.rb_leftmost) {
          struct sched_entity *left = rb_entry(cpu->eevfd.rb_leftmost, struct sched_entity, rb_node);
          if (prev->se.vruntime < left->vruntime + prev->se.slice_ns)
            prev->se.vruntime = left->vruntime + prev->se.slice_ns;
        }
        prev->se.deadline = calc_deadline(prev->se.vruntime, prev->se.slice_ns, prev->se.weight, prev->se.wmult);
        prev->se.min_deadline = prev->se.deadline;
      } else {
        /* Involuntary preemption/tick: only recalculate deadline if expired */
        if (prev->se.deadline <= prev->se.vruntime) {
          prev->se.deadline = calc_deadline(prev->se.vruntime, prev->se.slice_ns, prev->se.weight, prev->se.wmult);
          prev->se.min_deadline = prev->se.deadline;
        }
      }
      eevfd_enqueue_entity(&cpu->eevfd, &prev->se);
      prev->on_runqueue = true;
    }
  }

  // 2. Select earliest eligible virtual deadline task
  struct sched_entity *next_se = eevfd_pick_next_entity(&cpu->eevfd);
  struct thread *next_t = next_se ? rb_entry(next_se, struct thread, se) : NULL;

  // 2.5 Load Balancing (Work Stealing)
  if (!next_t) {
    sched_balance(cpu);
    next_se = eevfd_pick_next_entity(&cpu->eevfd);
    next_t = next_se ? rb_entry(next_se, struct thread, se) : NULL;
  }

  // 3. Perform the switch
  if (!next_t) {
    next_t = cpu->idle_thread;
  }

  if (next_t && !next_t->is_idle) {
    next_t->ready_since_ms = 0;
    eevfd_set_next_entity(&cpu->eevfd, &next_t->se);
    next_t->se.exec_start_ns = now_ns;
    next_t->on_runqueue = false;
  }

  sched_arm_next_deadline(cpu, next_t);

  if (next_t && next_t != prev) {
    if (!prev->is_idle && prev->state == THREAD_RUNNING) {
      prev->state = THREAD_READY;
      prev->ready_since_ms = now;
    }
    next_t->state = THREAD_RUNNING;
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


    if (prev->fs_base != next_t->fs_base)
      wrmsr(0xC0000100, next_t->fs_base);
    if (prev->gs_base != next_t->gs_base)
      wrmsr(0xC0000102, next_t->gs_base);

    spinlock_release(&cpu->queue_lock);
    switch_context(prev, next_t);
  } else {
    spinlock_release(&cpu->queue_lock);
  }

  __atomic_store_n(&cpu->switching_from, NULL, __ATOMIC_RELEASE);
  hal_irq_enable();
}

__attribute__((optimize("O3"))) void sched_yield(void) {
  sched_schedule(true);
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
    sched_yield();

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

__attribute__((optimize("O3"))) void sched_tick(struct registers *regs) {
  (void)regs;
  struct cpu_info *cpu = cpu_get_current();
  struct thread *curr = cpu->current_thread;
  if (curr) {
    uint64_t now = lapic_timer_get_ms();
    uint64_t now_ns = lapic_timer_get_ns();
    cpu->ticks = now;

    if (cpu == cpu_get_bsp()) {
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

    hal_irq_state_t flags = hal_irq_save();
    spinlock_acquire(&cpu->queue_lock);

    if (!curr->is_idle) {
      eevfd_update_curr(&cpu->eevfd, now_ns);
      curr->runtime_total = curr->se.prev_sum_exec_ns / 1000000ULL;
    }

    bool preempt = false;
    if (curr->is_idle && cpu->eevfd.nr_running > 0) {
      preempt = true;
    } else if (!curr->is_idle && cpu->eevfd.nr_running > 0) {
      uint64_t exec_delta = (curr->se.exec_start_ns && now_ns > curr->se.exec_start_ns)
                                ? (now_ns - curr->se.exec_start_ns)
                                : 0;
      if (curr->se.vruntime >= curr->se.deadline || exec_delta >= curr->se.slice_ns) {
        preempt = true;
      } else if (exec_delta >= EEVFD_MIN_GRANULARITY_NS && cpu->eevfd.rb_leftmost) {
        struct sched_entity *left = rb_entry(cpu->eevfd.rb_leftmost, struct sched_entity, rb_node);
        if (left && eevfd_check_preempt(&cpu->eevfd, &curr->se, left)) {
          preempt = true;
        }
      }
    }

    if (!preempt) {
      sched_arm_next_deadline(cpu, curr);
    }
    spinlock_release(&cpu->queue_lock);
    hal_irq_restore(flags);

    if (preempt) {
      sched_schedule(false);
    }
  }
}

struct thread *sched_get_current(void) {
  return cpu_get_current()->current_thread;
}

bool sched_validate_runqueues(struct cpu_info *cpu) {
  if (!cpu) return false;
  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&cpu->queue_lock);
  bool valid = eevfd_validate_rq(&cpu->eevfd);
  spinlock_release(&cpu->queue_lock);
  hal_irq_restore(flags);
  return valid;
}

bool sched_set_priority(struct thread *t, uint8_t priority, int8_t nice_value) {
  if (!t || t->is_idle) return false;
  struct cpu_info *cpu = cpu_get_info(t->cpu_index);
  if (!cpu || cpu->status == CPU_STATUS_OFFLINE) return false;
  hal_irq_state_t flags = hal_irq_save();
  spinlock_acquire(&cpu->queue_lock);
  t->static_priority = priority;
  t->priority = priority;
  t->nice_value = nice_value;
  if (t->se.on_rq) {
    eevfd_dequeue_entity(&cpu->eevfd, &t->se);
    eevfd_set_nice(&t->se, (int)nice_value);
    eevfd_enqueue_entity(&cpu->eevfd, &t->se);
  } else {
    eevfd_set_nice(&t->se, (int)nice_value);
  }
  spinlock_release(&cpu->queue_lock);
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
  console_puts("TID  CPU  NICE  WEIGHT  VRUNTIME(ms)  DEADLINE(ms)  STATE       COMM\n");
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    hal_irq_disable();
    spinlock_acquire(&cpu->queue_lock);

    struct rb_node *n = rb_first(&cpu->eevfd.tasks_tree);
    while (n) {
      struct sched_entity *se = rb_entry(n, struct sched_entity, rb_node);
      struct thread *curr = rb_entry(se, struct thread, se);

      klog_puts("TID: ");
      klog_uint64(curr->tid);
      klog_puts(" CPU: ");
      klog_uint64(cpu->cpu_id);
      klog_puts(" Nice: ");
      if (curr->nice_value < 0) {
        klog_puts("-");
        klog_uint64((uint64_t)(-curr->nice_value));
      } else {
        klog_uint64((uint64_t)curr->nice_value);
      }
      klog_puts(" W: ");
      klog_uint64(curr->se.weight);
      klog_puts(" V: ");
      klog_uint64(curr->se.vruntime / 1000000ULL);
      klog_puts(" D: ");
      klog_uint64(curr->se.deadline / 1000000ULL);
      klog_puts(" State: ");
      klog_puts(curr->state == THREAD_RUNNING ? "RUN " : "RDY ");
      klog_puts(" Comm: ");
      klog_puts(curr->comm);
      klog_puts("\n");

      n = rb_next(n);
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
__attribute__((unused)) static void remove_from_global_list(struct thread *t) {
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
  if (t->se.on_rq) {
    eevfd_dequeue_entity(&cpu_local->eevfd, &t->se);
    t->on_runqueue = false;
    if (cpu_local->runnable_count)
      cpu_local->runnable_count--;
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
      if (t->mm->pcid) {
        pcid_free(t->mm->pcid);
        t->mm->pcid = 0;
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
  /* Clear any per-CPU fpu_owner references pointing at this thread
     before the struct is freed, to avoid stale pointer dereferences. */
  fpu_forget_thread(t);
  kfree(t);

  klog_debug_puts("[REAP] Done\n");
}


static struct thread *find_thread_by_tid_locked(uint32_t tid) {
  struct thread *curr = global_thread_list;
  while (curr) {
    if (curr->tid == tid)
      return curr;
    curr = curr->global_next;
  }
  return NULL;
}

struct thread *sched_get_thread_by_tid(uint32_t tid) {
  spinlock_acquire(&tid_lock);
  struct thread *curr = find_thread_by_tid_locked(tid);
  spinlock_release(&tid_lock);
  return curr;
}

bool sched_get_thread_snapshot(uint32_t tid,
                               struct sched_thread_snapshot *snapshot) {
  if (!snapshot)
    return false;

  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (!t) {
    spinlock_release(&tid_lock);
    return false;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->tid = t->tid;
  snapshot->tgid = t->tgid;
  snapshot->parent_tid = t->parent ? t->parent->tid : 0;
  snapshot->pgid = t->pgid;
  snapshot->state = t->state;
  snapshot->runtime_total = t->runtime_total;
  snapshot->uid = t->uid;
  snapshot->gid = t->gid;
  snapshot->euid = t->euid;
  snapshot->egid = t->egid;
  snapshot->suid = t->suid;
  snapshot->sgid = t->sgid;
  memcpy(snapshot->comm, t->comm, sizeof(snapshot->comm));
  snapshot->comm[sizeof(snapshot->comm) - 1] = 0;

  if (t->mm) {
    if (t->mm->brk_current > t->mm->brk_base)
      snapshot->virt_bytes = t->mm->brk_current - t->mm->brk_base;
    uint64_t mmap_used = 0x800000000000ULL - t->mm->mmap_next_addr;
    if ((int64_t)mmap_used > 0)
      snapshot->virt_bytes += mmap_used;
    snapshot->resident_bytes = snapshot->virt_bytes / 2;
  }

  spinlock_release(&tid_lock);
  return true;
}

bool sched_get_nth_thread_tid(uint32_t index, uint32_t *tid) {
  if (!tid)
    return false;
  spinlock_acquire(&tid_lock);
  struct thread *t = global_thread_list;
  while (t && index--)
    t = t->global_next;
  if (t)
    *tid = t->tid;
  spinlock_release(&tid_lock);
  return t != NULL;
}

bool sched_get_nth_open_fd(uint32_t tid, uint32_t index, uint32_t *fd) {
  if (!fd)
    return false;
  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (!t || !t->files) {
    spinlock_release(&tid_lock);
    return false;
  }

  spinlock_acquire(&t->files->lock);
  bool found = false;
  for (uint32_t i = 0; i < MAX_FDS; i++) {
    if (!t->fds[i] || t->fds[i] == (vfs_node_t *)-1)
      continue;
    if (index-- == 0) {
      *fd = i;
      found = true;
      break;
    }
  }
  spinlock_release(&t->files->lock);
  spinlock_release(&tid_lock);
  return found;
}

bool sched_get_fd_path_snapshot(uint32_t tid, uint32_t fd, char *path,
                                size_t path_size) {
  if (!path || !path_size || fd >= MAX_FDS)
    return false;
  spinlock_acquire(&tid_lock);
  struct thread *t = find_thread_by_tid_locked(tid);
  if (!t || !t->files) {
    spinlock_release(&tid_lock);
    return false;
  }

  spinlock_acquire(&t->files->lock);
  vfs_node_t *node = t->fds[fd];
  bool found = node && node != (vfs_node_t *)-1;
  if (found) {
    const char *value =
        t->fd_paths[fd] && t->fd_paths[fd]->value[0]
            ? t->fd_paths[fd]->value
            : node->name;
    strncpy(path, value, path_size - 1);
    path[path_size - 1] = 0;
  }
  spinlock_release(&t->files->lock);
  spinlock_release(&tid_lock);
  return found;
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

uint16_t sched_get_runnable_thread_count(void) {
  spinlock_acquire(&tid_lock);
  uint16_t count = 0;
  for (struct thread *t = global_thread_list; t; t = t->global_next)
    if (t->state == THREAD_RUNNING || t->state == THREAD_READY)
      count++;
  spinlock_release(&tid_lock);
  return count;
}

struct thread *sched_get_thread_list_head(void) { return global_thread_list; }

__attribute__((optimize("O3"))) void sched_wakeup(struct thread *t) {
  if (!t)
    return;
  if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
    return; // Already runnable, nothing to do

  hal_irq_state_t rflags = hal_irq_save();

  struct cpu_info *target = cpu_get_info(t->cpu_index);
  if (target && target->status != CPU_STATUS_OFFLINE) {
    bool send_ipi = false;
    bool rearm_local = false;

    spinlock_acquire(&target->queue_lock);
    if (t->state != THREAD_READY && t->state != THREAD_RUNNING) {
      bool still_current =
          __atomic_load_n(&target->current_thread, __ATOMIC_ACQUIRE) == t;
      sched_deadline_remove_locked(target, t);
      t->state = still_current ? THREAD_RUNNING : THREAD_READY;
      t->wakeup_ticks = 0;
      if (!still_current) {
        eevfd_place_entity(&target->eevfd, &t->se, false);
        eevfd_enqueue_entity(&target->eevfd, &t->se);
        t->on_runqueue = true;
        target->runnable_count++;

        struct cpu_info *self = cpu_get_current();
        if (target->apic_id != self->apic_id) {
          if (!target->current_thread || target->current_thread->is_idle ||
              eevfd_check_preempt(&target->eevfd, &target->current_thread->se, &t->se)) {
            send_ipi = true;
          }
        } else if (self->current_thread &&
                   (self->current_thread->is_idle ||
                    eevfd_check_preempt(&self->eevfd, &self->current_thread->se, &t->se))) {
          rearm_local = true;
        }
      } else {
        t->ready_since_ms = 0;
      }
    }
    spinlock_release(&target->queue_lock);

    if (send_ipi) {
      lapic_send_ipi(target->apic_id, IPI_VECTOR_RESCHEDULE);
    } else if (rearm_local) {
      lapic_timer_rearm_if_earlier(lapic_timer_get_ms() + 1);
    }
  } else {
    t->state = THREAD_READY;
    t->wakeup_ticks = 0;
    sched_enqueue_thread(t, cpu_get_current());
  }

  hal_irq_restore(rflags);
}

