#ifndef __AVORY_LINUXKPI_KTHREAD_H
#define __AVORY_LINUXKPI_KTHREAD_H

/* Linux <linux/kthread.h> overlay.  Kernel threads are native scheduler
 * threads created through linuxkpi/native_sched.h; each carries a control
 * block holding the Linux threadfn/data and the cooperative stop flag. */

#include <linux/err.h>
#include <linux/sched.h>
#include <linux/types.h>

#define KTHREAD_NODE_ANY (-1)

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
                                           void *data, int node,
                                           const char namefmt[], ...);

#define kthread_create(threadfn, data, namefmt, arg...)                       \
  kthread_create_on_node(threadfn, data, KTHREAD_NODE_ANY, namefmt, ##arg)

#define kthread_run(threadfn, data, namefmt, ...)                             \
  ({                                                                          \
    struct task_struct *__k =                                                 \
        kthread_create(threadfn, data, namefmt, ##__VA_ARGS__);               \
    if (!IS_ERR(__k))                                                         \
      wake_up_process(__k);                                                   \
    __k;                                                                      \
  })

struct task_struct *kthread_run_on_cpu(int (*threadfn)(void *data), void *data,
                                       unsigned int cpu, const char *namefmt);

int kthread_stop(struct task_struct *k);
bool kthread_should_stop(void);
void kthread_bind(struct task_struct *k, unsigned int cpu);

#endif /* __AVORY_LINUXKPI_KTHREAD_H */
