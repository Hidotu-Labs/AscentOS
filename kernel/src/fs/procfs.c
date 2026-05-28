#include "fs/procfs.h"
#include "apic/lapic_timer.h"
#include "drivers/storage/block.h"
#include "drivers/timer/rtc.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "smp/cpu.h"

// Helper to convert an unsigned 64-bit integer to a string
static void u64_to_str(uint64_t val, char *buf) {
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char temp[32];
  int i = 0;
  while (val > 0) {
    temp[i++] = (val % 10) + '0';
    val /= 10;
  }
  int j = 0;
  while (i > 0) {
    buf[j++] = temp[--i];
  }
  buf[j] = '\0';
}

uint32_t procfs_meminfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  char buf[512];
  buf[0] = '\0';

  uint64_t usable_mem_val = pmm_get_usable_memory();
  uint64_t free_pages_val = (uint64_t)pmm_get_free_pages();
  uint64_t free_mem_val =
      free_pages_val * 4096; // Use literal if PAGE_SIZE is causing issues

  char num_buf[32];

  // MemTotal
  strcat(buf, "MemTotal:       ");
  u64_to_str(usable_mem_val / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");

  // MemFree
  strcat(buf, "MemFree:        ");
  u64_to_str(free_mem_val / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");

  // MemAvailable
  strcat(buf, "MemAvailable:   ");
  u64_to_str(free_mem_val / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");

  strcat(buf, "Buffers:        0 kB\n");
  strcat(buf, "Cached:         0 kB\n");

  // MemUsable
  strcat(buf, "MemUsable:      ");
  u64_to_str(usable_mem_val / 1024, num_buf);
  strcat(buf, num_buf);
  strcat(buf, " kB\n");

  uint32_t len = (uint32_t)strlen(buf);
  node->length = len;

  if (offset >= len)
    return 0;
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  return size;
}

uint32_t procfs_cpuinfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  // 16KB is plenty for 64 cores
  char *buf = kmalloc(16384);
  if (!buf)
    return 0;
  buf[0] = '\0';

  uint32_t cpu_count = cpu_get_count();
  char num_buf[32];

  for (uint32_t i = 0; i < cpu_count; i++) {
    strcat(buf, "processor       : ");
    u64_to_str(i, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");

    uint32_t eax, ebx, ecx, edx;

    // Vendor ID
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));
    char vendor[13];
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';
    strcat(buf, "vendor_id       : ");
    strcat(buf, vendor);
    strcat(buf, "\n");

    // Family, Model, Stepping
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    uint32_t stepping = eax & 0xF;
    uint32_t model = (eax >> 4) & 0xF;
    uint32_t family = (eax >> 8) & 0xF;
    if (family == 0xF)
      family += (eax >> 20) & 0xFF;
    if (family == 0x6 || family == 0xF)
      model += ((eax >> 16) & 0xF) << 4;

    strcat(buf, "cpu family      : ");
    u64_to_str(family, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");

    strcat(buf, "model           : ");
    u64_to_str(model, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");

    strcat(buf, "stepping        : ");
    u64_to_str(stepping, num_buf);
    strcat(buf, num_buf);
    strcat(buf, "\n");

    // Brand string (Model name)
    uint32_t brand_eax;
    __asm__ volatile("cpuid" : "=a"(brand_eax) : "a"(0x80000000));
    if (brand_eax >= 0x80000004) {
      char model_name[49];
      uint32_t *mptr = (uint32_t *)model_name;
      for (uint32_t j = 0; j < 3; j++) {
        __asm__ volatile("cpuid"
                         : "=a"(mptr[j * 4]), "=b"(mptr[j * 4 + 1]),
                           "=c"(mptr[j * 4 + 2]), "=d"(mptr[j * 4 + 3])
                         : "a"(0x80000002 + j));
      }
      model_name[48] = '\0';
      // Trim leading spaces
      char *trimmed = model_name;
      while (*trimmed == ' ')
        trimmed++;
      strcat(buf, "model name      : ");
      strcat(buf, trimmed);
      strcat(buf, "\n");
    }

    strcat(buf, "flags           : fpu vme de pse tsc msr pae mce cx8 apic sep "
                "mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse "
                "sse2 ss ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc "
                "art arch_perfmon pebs bts rep_good nopl cpuid nonstop_tsc "
                "cpuid_fault tpm tm2 est immortality sse3 pclmulqdq dtes64 "
                "monitor ds_cpl vmx smx est tm2 ssse3 sdbg fma cx16 xtpr pdcm "
                "pcid sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes "
                "xsave avx f16c rdrand lahf_lm abm 3dnowprefetch\n");
    strcat(buf, "\n");
  }

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_partitions_read(vfs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
  char *buf = kmalloc(4096);
  if (!buf)
    return 0;
  buf[0] = '\0';

  strcat(buf, "major minor  #blocks  name\n\n");

  int count = block_count();
  char num_buf[32];

  for (int i = 0; i < count; i++) {
    struct block_device *dev = block_get(i);
    if (!dev)
      continue;

    strcat(buf, "   1     ");
    u64_to_str(i, num_buf);
    strcat(buf, num_buf);

    int padding = 8 - strlen(num_buf);
    for (int j = 0; j < padding; j++)
      strcat(buf, " ");

    uint64_t blocks =
        (dev->total_sectors * (dev->sector_size ? dev->sector_size : 512)) /
        1024;
    u64_to_str(blocks, num_buf);
    strcat(buf, num_buf);

    padding = 10 - strlen(num_buf);
    for (int j = 0; j < padding; j++)
      strcat(buf, " ");

    strcat(buf, dev->name);
    strcat(buf, "\n");
  }

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_mounts_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  char *buf = kmalloc(512);
  if (!buf)
    return 0;
  buf[0] = '\0';

  strcat(buf, "/dev/sata01 / ext2 rw,relatime 0 0\n");
  strcat(buf, "proc /proc procfs rw,relatime 0 0\n");

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_uptime_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
  char buf[64];
  uint64_t ms = lapic_timer_get_ms();

  // Format: "up.time idle.time"
  u64_to_str(ms / 1000, buf);
  strcat(buf, ".");
  uint32_t rem = (ms % 1000) / 10;
  if (rem < 10)
    strcat(buf, "0");
  u64_to_str(rem, buf + strlen(buf));
  strcat(buf, " 0.00\n");

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len)
    return 0;
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  return size;
}

uint32_t procfs_stat_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
  char *buf = kmalloc(2048);
  if (!buf)
    return 0;
  buf[0] = '\0';

  // cpu  user nice system idle iowait irq softirq steal guest guest_nice
  // Distribute total uptime jiffies as idle across all CPUs.
  uint64_t ms      = lapic_timer_get_ms();
  uint64_t jiffies = ms / 10; // USER_HZ = 100
  uint32_t ncpus   = cpu_get_count();
  if (ncpus == 0) ncpus = 1;
  char num[32];

  // Aggregate cpu line: idle = jiffies * ncpus (sum across all CPUs)
  strcat(buf, "cpu  0 0 0 ");
  u64_to_str(jiffies * ncpus, num);
  strcat(buf, num);
  strcat(buf, " 0 0 0 0 0 0\n");

  // Per-CPU lines
  for (uint32_t i = 0; i < ncpus; i++) {
    strcat(buf, "cpu");
    u64_to_str(i, num);
    strcat(buf, num);
    strcat(buf, " 0 0 0 ");
    u64_to_str(jiffies, num);
    strcat(buf, num);
    strcat(buf, " 0 0 0 0 0 0\n");
  }

  strcat(buf, "intr 0\n");
  strcat(buf, "ctxt 0\n");

  // btime: Unix timestamp of boot (seconds since epoch)
  strcat(buf, "btime ");
  u64_to_str(rtc_get_boot_timestamp(), num);
  strcat(buf, num);
  strcat(buf, "\n");

  uint16_t nthreads = sched_get_thread_count();
  strcat(buf, "processes ");
  u64_to_str(nthreads, num);
  strcat(buf, num);
  strcat(buf, "\nprocs_running 1\nprocs_blocked 0\n");

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

// ── /proc/loadavg ─────────────────────────────────────────────────────────
// Format: "load1 load5 load15 running/total last_pid\n"
// htop parses this for the load average display.
// We approximate load as (running_threads / ncpus) clamped to a reasonable
// value, formatted as a fixed-point decimal (e.g. "0.42").
static void fmt_load(uint64_t running, uint64_t total_cpus, char *out) {
  // load = running / total_cpus, expressed as X.XX
  if (total_cpus == 0) total_cpus = 1;
  uint64_t integer = running / total_cpus;
  uint64_t frac    = (running * 100 / total_cpus) % 100;
  char tmp[8];
  u64_to_str(integer, out);
  strcat(out, ".");
  if (frac < 10) strcat(out, "0");
  u64_to_str(frac, tmp);
  strcat(out, tmp);
}

uint32_t procfs_loadavg_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  char buf[128];
  buf[0] = '\0';

  uint32_t ncpus    = cpu_get_count();
  if (ncpus == 0) ncpus = 1;
  uint16_t nthreads = sched_get_thread_count();

  // Count running threads
  uint32_t running = 0;
  struct thread *t = sched_get_thread_list_head();
  while (t) {
    if (t->state == THREAD_RUNNING || t->state == THREAD_READY)
      running++;
    t = t->global_next;
  }
  if (running == 0) running = 1;

  char load[16];
  fmt_load(running, ncpus, load);

  // load1 load5 load15 running/total last_pid
  strcat(buf, load);
  strcat(buf, " ");
  strcat(buf, load);
  strcat(buf, " ");
  strcat(buf, load);
  strcat(buf, " ");

  char tmp[16];
  u64_to_str(running, tmp);
  strcat(buf, tmp);
  strcat(buf, "/");
  u64_to_str(nthreads, tmp);
  strcat(buf, tmp);
  strcat(buf, " 1\n");

  uint32_t len = (uint32_t)strlen(buf);
  node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

uint32_t procfs_heapinfo_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
  // 4KB should be plenty for heap info
  char *buf = kmalloc(4096);
  if (!buf)
    return 0;

  heap_get_info(buf);

  uint32_t len = strlen(buf);
  node->length = len;

  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

uint32_t procfs_cmdline_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  (void)node;
  const char *cmd = "Xfbdev\n";
  uint32_t len = (uint32_t)strlen(cmd);

  if (offset >= len)
    return 0;
  if (offset + size > len) {
    size = len - offset;
  }
  memcpy(buffer, cmd + offset, size);
  return size;
}

// ── Dynamic per-PID /proc/<pid>/ support ─────────────────────────────────
//
// Rather than pre-creating directories at boot (processes come and go), we
// install custom readdir/finddir hooks on the procfs root that synthesise
// PID entries on the fly by walking the live scheduler thread list.
//
// /proc/<pid>/stat   – the primary file ps(1) reads
// /proc/<pid>/status – human-readable status (optional but helpful)
// /proc/<pid>/cmdline – argv[0] of the process

// ── Helpers ──────────────────────────────────────────────────────────────

static void pid_u32_to_str(uint32_t val, char *buf) {
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char tmp[16];
  int i = 0;
  while (val > 0) {
    tmp[i++] = (char)('0' + val % 10);
    val /= 10;
  }
  int j = 0;
  while (i > 0)
    buf[j++] = tmp[--i];
  buf[j] = '\0';
}

// Parse a decimal string; returns 0 if not a pure number.
static uint32_t str_to_pid(const char *s) {
  if (!s || !*s)
    return 0;
  uint32_t v = 0;
  for (const char *p = s; *p; p++) {
    if (*p < '0' || *p > '9')
      return 0;
    v = v * 10 + (uint32_t)(*p - '0');
  }
  return v;
}

// Map thread_state_t to the single-char Linux stat state.
static char thread_state_char(thread_state_t s) {
  switch (s) {
  case THREAD_RUNNING:  return 'R';
  case THREAD_READY:    return 'R';
  case THREAD_BLOCKED:  return 'S';
  case THREAD_SLEEPING: return 'S';
  case THREAD_DEAD:     return 'Z';
  case THREAD_ZOMBIE:   return 'Z';
  default:              return 'S';
  }
}

// ── /proc/<pid>/stat read ─────────────────────────────────────────────────
//
// Linux /proc/<pid>/stat format (fields 1-52, space-separated):
//   pid (comm) state ppid pgrp session tty_nr ...
// ps(1) needs fields 1-5 and 14 (utime) + 15 (stime) in USER_HZ jiffies.

static uint32_t procfs_pid_stat_read(vfs_node_t *node, uint32_t offset,
                                     uint32_t size, uint8_t *buffer) {
  // The PID is stashed in node->impl
  uint32_t pid = node->impl;
  struct thread *t = sched_get_thread_by_tid(pid);
  if (!t)
    return 0;

  char buf[512];
  buf[0] = '\0';

  char num[32];

  // Field 1: pid
  pid_u32_to_str(pid, num);
  strcat(buf, num);
  strcat(buf, " ");

  // Field 2: comm (executable name in parens, max 15 chars)
  strcat(buf, "(");
  if (t->comm[0])
    strcat(buf, t->comm);
  else
    strcat(buf, "unknown");
  strcat(buf, ") ");

  // Field 3: state
  char sc[3] = {thread_state_char(t->state), ' ', '\0'};
  strcat(buf, sc);

  // Field 4: ppid
  uint32_t ppid = t->parent ? t->parent->tid : 0;
  pid_u32_to_str(ppid, num);
  strcat(buf, num);
  strcat(buf, " ");

  // Field 5: pgrp
  pid_u32_to_str(t->pgid, num);
  strcat(buf, num);
  strcat(buf, " ");

  // Fields 6-13: stub zeros (session tty_nr tpgid flags minflt cminflt majflt cmajflt)
  strcat(buf, "0 0 0 0 0 0 0 0 ");

  // Field 14: utime (USER_HZ jiffies; LAPIC at 1000 Hz → divide by 10)
  uint64_t jiffies = t->runtime_total / 10;
  pid_u32_to_str((uint32_t)jiffies, num);
  strcat(buf, num);
  strcat(buf, " ");

  // Field 15: stime (kernel time — stub 0, we don't separate user/kernel)
  strcat(buf, "0 ");

  // Fields 16-52: stub zeros
  strcat(buf, "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
              "0 0 0 0 0 0 0\n");

  uint32_t len = (uint32_t)strlen(buf);
  node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

// ── /proc/<pid>/status read ───────────────────────────────────────────────

static uint32_t procfs_pid_status_read(vfs_node_t *node, uint32_t offset,
                                       uint32_t size, uint8_t *buffer) {
  uint32_t pid = node->impl;
  struct thread *t = sched_get_thread_by_tid(pid);
  if (!t)
    return 0;

  char *buf = kmalloc(768);
  if (!buf)
    return 0;
  buf[0] = '\0';

  char num[32];

  strcat(buf, "Name:\t");
  if (t->comm[0])
    strcat(buf, t->comm);
  else
    strcat(buf, "unknown");
  strcat(buf, "\nState:\t");
  char sc[2] = {thread_state_char(t->state), '\0'};
  strcat(buf, sc);
  strcat(buf, "\nTgid:\t");
  pid_u32_to_str(pid, num);
  strcat(buf, num);
  strcat(buf, "\nPid:\t");
  pid_u32_to_str(pid, num);
  strcat(buf, num);
  strcat(buf, "\nPPid:\t");
  pid_u32_to_str(t->parent ? t->parent->tid : 0, num);
  strcat(buf, num);
  strcat(buf, "\nUid:\t");
  pid_u32_to_str(t->uid, num);  strcat(buf, num); strcat(buf, "\t");
  pid_u32_to_str(t->euid, num); strcat(buf, num); strcat(buf, "\t");
  pid_u32_to_str(t->suid, num); strcat(buf, num); strcat(buf, "\t");
  pid_u32_to_str(t->uid, num);  strcat(buf, num);
  strcat(buf, "\nGid:\t");
  pid_u32_to_str(t->gid, num);  strcat(buf, num); strcat(buf, "\t");
  pid_u32_to_str(t->egid, num); strcat(buf, num); strcat(buf, "\t");
  pid_u32_to_str(t->sgid, num); strcat(buf, num); strcat(buf, "\t");
  pid_u32_to_str(t->gid, num);  strcat(buf, num);
  strcat(buf, "\nThreads:\t1\n");

  // Memory fields (kB) — htop reads VmSize and VmRSS
  uint64_t virt_kb = 2048; // 2 MB default
  uint64_t rss_kb  = 512;
  if (t->mm) {
    uint64_t virt_bytes = 0;
    if (t->mm->brk_current > t->mm->brk_base)
      virt_bytes += t->mm->brk_current - t->mm->brk_base;
    if (virt_bytes < 2 * 1024 * 1024)
      virt_bytes = 2 * 1024 * 1024;
    virt_kb = virt_bytes / 1024;
    rss_kb  = virt_kb / 4;
    if (rss_kb < 512) rss_kb = 512;
  }
  strcat(buf, "VmSize:\t");
  pid_u32_to_str((uint32_t)virt_kb, num); strcat(buf, num);
  strcat(buf, " kB\nVmRSS:\t");
  pid_u32_to_str((uint32_t)rss_kb, num);  strcat(buf, num);
  strcat(buf, " kB\n");

  uint32_t len = (uint32_t)strlen(buf);
  node->length = len;
  if (offset >= len) {
    kfree(buf);
    return 0;
  }
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, buf + offset, size);
  kfree(buf);
  return size;
}

// ── /proc/<pid>/cmdline read ──────────────────────────────────────────────

static uint32_t procfs_pid_cmdline_read(vfs_node_t *node, uint32_t offset,
                                        uint32_t size, uint8_t *buffer) {
  uint32_t pid = node->impl;
  struct thread *t = sched_get_thread_by_tid(pid);
  if (!t)
    return 0;

  // Return comm as argv[0] (NUL-terminated, as Linux does)
  const char *cmd = t->comm[0] ? t->comm : "unknown";
  uint32_t len = (uint32_t)strlen(cmd) + 1; // include NUL
  node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, cmd + offset, size);
  return size;
}

// ── /proc/<pid>/statm read ────────────────────────────────────────────────
// Format: "size resident shared text lib data dt\n"
// All values in pages (4096 bytes). htop uses this for VIRT/RES columns.
static uint32_t procfs_pid_statm_read(vfs_node_t *node, uint32_t offset,
                                      uint32_t size, uint8_t *buffer) {
  uint32_t pid = node->impl;
  struct thread *t = sched_get_thread_by_tid(pid);
  if (!t)
    return 0;

  char buf[64];
  buf[0] = '\0';

  // Estimate virtual size from mm->brk_current and mmap region
  uint64_t virt_bytes = 0;
  uint64_t res_bytes  = 0;
  if (t->mm) {
    // brk region
    if (t->mm->brk_current > t->mm->brk_base)
      virt_bytes += t->mm->brk_current - t->mm->brk_base;
    // mmap region (rough: distance from mmap base to next alloc)
    uint64_t mmap_used = 0x800000000000ULL - t->mm->mmap_next_addr;
    if ((int64_t)mmap_used > 0)
      virt_bytes += mmap_used;
    // Resident: assume half of virtual as a rough estimate
    res_bytes = virt_bytes / 2;
  }
  // Add a base for the stack + code (2 MB minimum so htop shows something)
  if (virt_bytes < 2 * 1024 * 1024)
    virt_bytes = 2 * 1024 * 1024;
  if (res_bytes < 512 * 1024)
    res_bytes = 512 * 1024;

  uint64_t virt_pages = virt_bytes / 4096;
  uint64_t res_pages  = res_bytes  / 4096;

  char num[32];
  // size resident shared text lib data dt
  u64_to_str(virt_pages, num); strcat(buf, num); strcat(buf, " ");
  u64_to_str(res_pages,  num); strcat(buf, num); strcat(buf, " ");
  strcat(buf, "0 0 0 ");
  u64_to_str(virt_pages, num); strcat(buf, num);
  strcat(buf, " 0\n");

  uint32_t len = (uint32_t)strlen(buf);
  node->length = len;
  if (offset >= len)
    return 0;
  if (offset + size > len)
    size = len - offset;
  memcpy(buffer, buf + offset, size);
  return size;
}

// ── /proc/<pid>/io read ───────────────────────────────────────────────────
// htop 3.x reads this for I/O accounting. Stub with zeros.
static uint32_t procfs_pid_io_read(vfs_node_t *node, uint32_t offset,
                                   uint32_t size, uint8_t *buffer) {
  (void)node;
  const char *io =
    "rchar: 0\n"
    "wchar: 0\n"
    "syscr: 0\n"
    "syscw: 0\n"
    "read_bytes: 0\n"
    "write_bytes: 0\n"
    "cancelled_write_bytes: 0\n";
  uint32_t len = (uint32_t)strlen(io);
  if (offset >= len) return 0;
  if (offset + size > len) size = len - offset;
  memcpy(buffer, io + offset, size);
  return size;
}

// ── Synthesise a /proc/<pid>/ directory node on demand ───────────────────

static vfs_node_t *make_pid_dir(uint32_t pid) {
  vfs_node_t *dir = kmalloc(sizeof(vfs_node_t));
  if (!dir)
    return NULL;
  vfs_node_init(dir);
  pid_u32_to_str(pid, dir->name);
  dir->flags = FS_DIRECTORY; // not FS_PERSISTENT — ephemeral
  dir->mask  = 0555;
  dir->inode = 0x10000 + pid;
  ramfs_mount_on(dir);

  // stat
  vfs_node_t *stat_node = kmalloc(sizeof(vfs_node_t));
  if (stat_node) {
    vfs_node_init(stat_node);
    strcpy(stat_node->name, "stat");
    stat_node->flags  = FS_FILE;
    stat_node->mask   = 0444;
    stat_node->impl   = pid; // stash PID for the read callback
    stat_node->length = 128;
    stat_node->read   = procfs_pid_stat_read;
    ramfs_mount_node(dir, stat_node);
  }

  // status
  vfs_node_t *status_node = kmalloc(sizeof(vfs_node_t));
  if (status_node) {
    vfs_node_init(status_node);
    strcpy(status_node->name, "status");
    status_node->flags  = FS_FILE;
    status_node->mask   = 0444;
    status_node->impl   = pid;
    status_node->length = 256;
    status_node->read   = procfs_pid_status_read;
    ramfs_mount_node(dir, status_node);
  }

  // cmdline
  vfs_node_t *cmdline_node = kmalloc(sizeof(vfs_node_t));
  if (cmdline_node) {
    vfs_node_init(cmdline_node);
    strcpy(cmdline_node->name, "cmdline");
    cmdline_node->flags  = FS_FILE;
    cmdline_node->mask   = 0444;
    cmdline_node->impl   = pid;
    cmdline_node->length = 256;
    cmdline_node->read   = procfs_pid_cmdline_read;
    ramfs_mount_node(dir, cmdline_node);
  }

  // statm — memory usage in pages (VIRT/RES for htop)
  vfs_node_t *statm_node = kmalloc(sizeof(vfs_node_t));
  if (statm_node) {
    vfs_node_init(statm_node);
    strcpy(statm_node->name, "statm");
    statm_node->flags  = FS_FILE;
    statm_node->mask   = 0444;
    statm_node->impl   = pid;
    statm_node->length = 64;
    statm_node->read   = procfs_pid_statm_read;
    ramfs_mount_node(dir, statm_node);
  }

  // io — I/O stats stub (htop 3.x tries to open this)
  vfs_node_t *io_node = kmalloc(sizeof(vfs_node_t));
  if (io_node) {
    vfs_node_init(io_node);
    strcpy(io_node->name, "io");
    io_node->flags  = FS_FILE;
    io_node->mask   = 0444;
    io_node->impl   = pid;
    io_node->length = 64;
    io_node->read   = procfs_pid_io_read;
    ramfs_mount_node(dir, io_node);
  }

  return dir;
}

// ── Number of static entries in the procfs root (excluding . and ..) ─────
// These are the nodes added by procfs_init before we install our hooks:
//   meminfo cpuinfo partitions mounts uptime stat heapinfo cmdline loadavg self → 10
#define PROCFS_STATIC_ENTRIES 10

// ── Custom readdir for /proc ──────────────────────────────────────────────
//
// Index layout:
//   0        → "."
//   1        → ".."
//   2..N+1   → static ramfs children (N = PROCFS_STATIC_ENTRIES)
//   N+2 ..   → live PID entries (one per thread in global_thread_list)

static struct dirent procfs_dent; // single static buffer (safe: no preemption
                                  // between readdir calls in getdents64 loop)

static struct dirent *procfs_root_readdir(vfs_node_t *node, uint32_t index) {
  memset(&procfs_dent, 0, sizeof(procfs_dent));

  if (index == 0) {
    strcpy(procfs_dent.name, ".");
    procfs_dent.ino = node->inode;
    return &procfs_dent;
  }
  if (index == 1) {
    strcpy(procfs_dent.name, "..");
    procfs_dent.ino = node->inode;
    return &procfs_dent;
  }

  // Static children (index 2 .. PROCFS_STATIC_ENTRIES+1)
  uint32_t static_idx = index - 2;
  if (static_idx < PROCFS_STATIC_ENTRIES) {
    // Walk the ramfs child list
    typedef struct child_node_s { vfs_node_t *node; struct child_node_s *next; } child_node_t;
    typedef struct { child_node_t *children; } ramfs_dir_t;
    ramfs_dir_t *rdir = (ramfs_dir_t *)node->device;
    if (!rdir)
      return NULL;
    child_node_t *curr = rdir->children;
    for (uint32_t i = 0; i < static_idx && curr; i++)
      curr = curr->next;
    if (!curr)
      return NULL;
    strcpy(procfs_dent.name, curr->node->name);
    procfs_dent.ino = curr->node->inode;
    return &procfs_dent;
  }

  // PID entries
  uint32_t pid_idx = static_idx - PROCFS_STATIC_ENTRIES;
  struct thread *t = sched_get_thread_list_head();
  uint32_t i = 0;
  while (t) {
    if (i == pid_idx) {
      pid_u32_to_str(t->tid, procfs_dent.name);
      procfs_dent.ino = 0x10000 + t->tid;
      return &procfs_dent;
    }
    i++;
    t = t->global_next;
  }

  return NULL; // end of directory
}

// ── Custom finddir for /proc ──────────────────────────────────────────────

static vfs_node_t *procfs_root_finddir(vfs_node_t *node, char *name) {
  // First try the static ramfs children
  typedef struct child_node_s { vfs_node_t *node; struct child_node_s *next; } child_node_t;
  typedef struct { child_node_t *children; } ramfs_dir_t;
  ramfs_dir_t *rdir = (ramfs_dir_t *)node->device;
  if (rdir) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      return node;
    child_node_t *curr = rdir->children;
    while (curr) {
      if (strcmp(curr->node->name, name) == 0)
        return curr->node;
      curr = curr->next;
    }
  }

  // Check if it's a numeric PID
  uint32_t pid = str_to_pid(name);
  if (pid == 0)
    return NULL;

  // Verify the thread actually exists
  struct thread *t = sched_get_thread_by_tid(pid);
  if (!t)
    return NULL;

  // Synthesise a fresh directory node for this PID
  return make_pid_dir(pid);
}

void procfs_init(void) {
  if (!fs_root)
    return;

  // Try to find /proc, create if it doesn't exist (assuming ext2/3 supports
  // root->mkdir)
  vfs_node_t *proc_dir = vfs_finddir(fs_root, "proc");
  if (!proc_dir && fs_root->mkdir) {
    fs_root->mkdir(fs_root, "proc", 0755);
    proc_dir = vfs_finddir(fs_root, "proc");
  }

  if (proc_dir) {
    // Stage a new virtual root for procfs
    vfs_node_t *procfs_root = kmalloc(sizeof(vfs_node_t));
    vfs_node_init(procfs_root);
    strcpy(procfs_root->name, "proc");
    procfs_root->flags = FS_DIRECTORY;
    procfs_root->mask  = 0555;
    ramfs_mount_on(procfs_root);

    // Apply the mount: anyone looking up 'proc' will now get our virtual root
    vfs_mount(proc_dir, procfs_root);

    // Add /proc/meminfo
    vfs_node_t *meminfo_node = kmalloc(sizeof(vfs_node_t));
    if (meminfo_node) {
      vfs_node_init(meminfo_node);
      strncpy(meminfo_node->name, "meminfo", 127);
      meminfo_node->flags = FS_FILE | FS_PERSISTENT;
      meminfo_node->mask = 0444; // Read-only
      meminfo_node->read = procfs_meminfo_read;
      meminfo_node->length = 512; // Dummy size, redefined on read

      ramfs_mount_node(procfs_root, meminfo_node);
    }

    // Add /proc/cpuinfo
    vfs_node_t *cpuinfo_node = kmalloc(sizeof(vfs_node_t));
    if (cpuinfo_node) {
      vfs_node_init(cpuinfo_node);
      strncpy(cpuinfo_node->name, "cpuinfo", 127);
      cpuinfo_node->flags = FS_FILE | FS_PERSISTENT;
      cpuinfo_node->mask = 0444; // Read-only
      cpuinfo_node->read = procfs_cpuinfo_read;
      cpuinfo_node->length = 2048; // Dummy size

      ramfs_mount_node(procfs_root, cpuinfo_node);
    }

    // Add /proc/partitions
    vfs_node_t *part_node = kmalloc(sizeof(vfs_node_t));
    if (part_node) {
      vfs_node_init(part_node);
      strncpy(part_node->name, "partitions", 127);
      part_node->flags = FS_FILE | FS_PERSISTENT;
      part_node->mask = 0444; // Read-only
      part_node->read = procfs_partitions_read;
      part_node->length = 1024; // Dummy size

      ramfs_mount_node(procfs_root, part_node);
    }

    // Add /proc/mounts
    vfs_node_t *mounts_node = kmalloc(sizeof(vfs_node_t));
    if (mounts_node) {
      vfs_node_init(mounts_node);
      strncpy(mounts_node->name, "mounts", 127);
      mounts_node->flags = FS_FILE | FS_PERSISTENT;
      mounts_node->mask = 0444;
      mounts_node->read = procfs_mounts_read;
      ramfs_mount_node(procfs_root, mounts_node);
    }

    // Add /proc/uptime
    vfs_node_t *uptime_node = kmalloc(sizeof(vfs_node_t));
    if (uptime_node) {
      vfs_node_init(uptime_node);
      strncpy(uptime_node->name, "uptime", 127);
      uptime_node->flags = FS_FILE | FS_PERSISTENT;
      uptime_node->mask = 0444;
      uptime_node->read = procfs_uptime_read;
      ramfs_mount_node(procfs_root, uptime_node);
    }

    // Add /proc/stat
    vfs_node_t *static_node = kmalloc(sizeof(vfs_node_t));
    if (static_node) {
      vfs_node_init(static_node);
      strncpy(static_node->name, "stat", 127);
      static_node->flags = FS_FILE | FS_PERSISTENT;
      static_node->mask = 0444;
      static_node->read = procfs_stat_read;
      ramfs_mount_node(procfs_root, static_node);
    }

    // Add /proc/loadavg
    vfs_node_t *loadavg_node = kmalloc(sizeof(vfs_node_t));
    if (loadavg_node) {
      vfs_node_init(loadavg_node);
      strncpy(loadavg_node->name, "loadavg", 127);
      loadavg_node->flags = FS_FILE | FS_PERSISTENT;
      loadavg_node->mask = 0444;
      loadavg_node->read = procfs_loadavg_read;
      ramfs_mount_node(procfs_root, loadavg_node);
    }

    // Add /proc/heapinfo
    vfs_node_t *heapinfo_node = kmalloc(sizeof(vfs_node_t));
    if (heapinfo_node) {
      vfs_node_init(heapinfo_node);
      strncpy(heapinfo_node->name, "heapinfo", 127);
      heapinfo_node->flags = FS_FILE | FS_PERSISTENT;
      heapinfo_node->mask = 0444;
      heapinfo_node->read = procfs_heapinfo_read;
      ramfs_mount_node(procfs_root, heapinfo_node);
    }

    // Add /proc/cmdline
    vfs_node_t *cmdline_node = kmalloc(sizeof(vfs_node_t));
    if (cmdline_node) {
      vfs_node_init(cmdline_node);
      strncpy(cmdline_node->name, "cmdline", 127);
      cmdline_node->flags = FS_FILE | FS_PERSISTENT;
      cmdline_node->mask = 0444;
      cmdline_node->read = procfs_cmdline_read;
      ramfs_mount_node(procfs_root, cmdline_node);
    }

    // Add /proc/self directory
    vfs_node_t *self_dir = kmalloc(sizeof(vfs_node_t));
    if (self_dir) {
      vfs_node_init(self_dir);
      strncpy(self_dir->name, "self", 127);
      self_dir->flags = FS_DIRECTORY | FS_PERSISTENT;
      self_dir->mask = 0555;
      ramfs_mount_on(self_dir); // Crucial: Initialize directory structure!
      ramfs_mount_node(procfs_root, self_dir);

      // Add /proc/self/cmdline
      vfs_node_t *self_cmdline = kmalloc(sizeof(vfs_node_t));
      if (self_cmdline) {
        vfs_node_init(self_cmdline);
        strncpy(self_cmdline->name, "cmdline", 127);
        self_cmdline->flags = FS_FILE | FS_PERSISTENT;
        self_cmdline->mask = 0444;
        self_cmdline->read = procfs_cmdline_read;
        ramfs_mount_node(self_dir, self_cmdline);
      }
    }

    // Install dynamic PID hooks on top of the ramfs root.
    // These wrap the ramfs readdir/finddir to also expose live per-PID dirs.
    procfs_root->readdir = procfs_root_readdir;
    procfs_root->finddir = procfs_root_finddir;
  }
}
