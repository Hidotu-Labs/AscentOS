/* LinuxKPI kernel threads.  See linux/kthread.h and the native bridge
 * linuxkpi/native_sched.h.
 *
 * Each thread carries a control block (magic-tagged, stored in the native
 * thread's kpi_data) with the Linux threadfn/data and the cooperative stop
 * flag.  Blocks are freed by kthread_stop(); threads that are never stopped
 * keep their (small) control block for the thread's lifetime. */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stdarg.h>

#include <linuxkpi/log.h>
#include <linuxkpi/native_sched.h>

#define KPI_KTHREAD_MAGIC 0x4b544852u /* "KTHR" */

struct kpi_kthread {
  unsigned int magic;
  int (*threadfn)(void *data);
  void *data;
  volatile int should_stop;
  struct completion exited;
};

static void kpi_kthread_trampoline(void *arg) {
  struct kpi_kthread *k = arg;

  k->threadfn(k->data);
  complete(&k->exited);
}

static struct kpi_kthread *kthread_control(struct task_struct *task) {
  struct kpi_kthread *k = linuxkpi_thread_data((void *)task);

  if (k && k->magic == KPI_KTHREAD_MAGIC)
    return k;
  return NULL;
}

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
                                           void *data, int node,
                                           const char namefmt[], ...) {
  char name[16];
  va_list args;

  (void)node;

  va_start(args, namefmt);
  vsnprintf(name, sizeof(name), namefmt, args);
  va_end(args);

  struct kpi_kthread *k = kmalloc(sizeof(*k), GFP_KERNEL);
  if (!k)
    return ERR_PTR(-ENOMEM);

  k->magic = KPI_KTHREAD_MAGIC;
  k->threadfn = threadfn;
  k->data = data;
  k->should_stop = 0;
  init_completion(&k->exited);

  void *thread = linuxkpi_kthread_create(kpi_kthread_trampoline, k, name);
  if (!thread) {
    kfree(k);
    return ERR_PTR(-ENOMEM);
  }

  return (struct task_struct *)thread;
}

struct task_struct *kthread_run_on_cpu(int (*threadfn)(void *data), void *data,
                                       unsigned int cpu, const char *namefmt) {
  (void)cpu;
  struct task_struct *task = kthread_create(threadfn, data, "%s", namefmt);
  if (!IS_ERR(task))
    wake_up_process(task);
  return task;
}

int kthread_stop(struct task_struct *task) {
  struct kpi_kthread *k = kthread_control(task);

  if (!k)
    return -EINVAL;

  k->should_stop = 1;
  linuxkpi_wake_thread((void *)task);

  wait_for_completion(&k->exited);
  kfree(k);
  return 0;
}

bool kthread_should_stop(void) {
  struct kpi_kthread *k = linuxkpi_thread_self_data();

  return k && k->magic == KPI_KTHREAD_MAGIC && k->should_stop;
}

void kthread_bind(struct task_struct *k, unsigned int cpu) {
  (void)k;
  (void)cpu;
}
