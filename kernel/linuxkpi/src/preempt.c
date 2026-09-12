/* Per-CPU preemption counter backing the LinuxKPI <linux/preempt.h>.
 * See linuxkpi/include/linux/preempt.h for the contract. */

#include <linux/preempt.h>

#include <linuxkpi/native.h>

/* Native MAX_CPUS is 64 (kernel/src/smp/cpu.h); keep in sync via
 * linuxkpi_max_cpus() at runtime for any future growth. */
#define KPI_MAX_CPUS 64

static unsigned int preempt_counts[KPI_MAX_CPUS];

unsigned int __kpi_preempt_count(void) {
  int cpu = linuxkpi_cpu_id();
  return __atomic_load_n(&preempt_counts[cpu], __ATOMIC_RELAXED);
}

void __kpi_preempt_add(int val) {
  int cpu = linuxkpi_cpu_id();
  __atomic_add_fetch(&preempt_counts[cpu], (unsigned int)val, __ATOMIC_ACQ_REL);
}

void __kpi_preempt_sub(int val) {
  int cpu = linuxkpi_cpu_id();
  unsigned int cur = __atomic_load_n(&preempt_counts[cpu], __ATOMIC_RELAXED);

  if (cur >= (unsigned int)val)
    __atomic_sub_fetch(&preempt_counts[cpu], (unsigned int)val,
                       __ATOMIC_ACQ_REL);
}

void __kpi_preempt_disable(void) { __kpi_preempt_add(1); }

void __kpi_preempt_enable(void) { __kpi_preempt_sub(1); }
