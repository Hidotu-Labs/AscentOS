// ── Architecture Syscalls: arch_prctl ───────────────────────────────────────
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../cpu/msr.h"
#include "../drivers/timer/rtc.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

// ── arch_prctl sub-commands (Linux x86_64) ──────────────────────────────────
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

// ── MSR addresses ───────────────────────────────────────────────────────────
#define IA32_FS_BASE 0xC0000100
#define IA32_GS_BASE 0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

static int is_canonical_user_addr(uint64_t addr) {
  uint64_t sign = (addr >> 47) & 1ULL;
  uint64_t upper = addr >> 48;
  return (sign == 0) ? (upper == 0) : (upper == 0xFFFF);
}

// ── sys_arch_prctl ──────────────────────────────────────────────────────────
// Linux ABI: arch_prctl(code, addr)
//   rdi = code, rsi = addr
static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  switch (code) {
  case ARCH_SET_FS:
    if (!is_canonical_user_addr(addr)) {
      klog_puts("[ARCH_PRCTL] Reject non-canonical SET_FS = ");
      klog_uint64(addr);
      klog_puts("\n");
      return (uint64_t)-22; // -EINVAL
    }
    wrmsr(IA32_FS_BASE, addr);
    {
      extern struct thread *sched_get_current(void);
      struct thread *cur = sched_get_current();
      if (cur)
        cur->fs_base = addr;
    }
    klog_puts("[ARCH_PRCTL] SET_FS = ");
    klog_uint64(addr);
    klog_puts("\n");
    return 0;

  case ARCH_GET_FS:
    // addr is a pointer to a uint64_t in userspace where we store the value.
    if (addr) {
      if (!vmm_is_user_addr_range_valid(addr, sizeof(uint64_t)))
        return (uint64_t)-14;
      *(uint64_t *)addr = rdmsr(IA32_FS_BASE);
    }
    return 0;

  case ARCH_SET_GS:
    if (!is_canonical_user_addr(addr)) {
      klog_puts("[ARCH_PRCTL] Reject non-canonical SET_GS = ");
      klog_uint64(addr);
      klog_puts("\n");
      return (uint64_t)-22; // -EINVAL
    }
    // For user GS, we write to KERNEL_GS_BASE (swapgs swaps it in/out).
    // After sysret + swapgs, this becomes the active GS for userspace.
    wrmsr(IA32_KERNEL_GS_BASE, addr);
    klog_puts("[ARCH_PRCTL] SET_GS = ");
    klog_uint64(addr);
    klog_puts("\n");
    return 0;

  case ARCH_GET_GS:
    if (addr) {
      if (!vmm_is_user_addr_range_valid(addr, sizeof(uint64_t)))
        return (uint64_t)-14;
      *(uint64_t *)addr = rdmsr(IA32_KERNEL_GS_BASE);
    }
    return 0;

  default:
    klog_puts("[ARCH_PRCTL] Unknown code: ");
    klog_uint64(code);
    klog_puts("\n");
    return (uint64_t)-22; // -EINVAL
  }
}

static uint64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_ptr, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!tp_ptr)
    return (uint64_t)-14; // EFAULT

  uint64_t ms = lapic_timer_get_ms();
  uint64_t sec = ms / 1000;
  uint64_t nsec = (ms % 1000) * 1000000ULL;

  switch (clk_id) {
  case 0: // CLOCK_REALTIME
  case 4: // CLOCK_REALTIME_COARSE (map to REALTIME)
    ((uint64_t *)tp_ptr)[0] = rtc_get_boot_timestamp() + sec;
    ((uint64_t *)tp_ptr)[1] = nsec;
    return 0;
  case 1: // CLOCK_MONOTONIC
  case 6: // CLOCK_MONOTONIC_COARSE (map to monotonic)
  case 7: // CLOCK_BOOTTIME (map to monotonic for simplicity)
  case 11: // CLOCK_TAI (map to monotonic)
    ((uint64_t *)tp_ptr)[0] = sec;
    ((uint64_t *)tp_ptr)[1] = nsec;
    return 0;
  case 2: // CLOCK_PROCESS_CPUTIME_ID - use monotonic time
  case 3: // CLOCK_THREAD_CPUTIME_ID - use monotonic time
    ((uint64_t *)tp_ptr)[0] = sec;
    ((uint64_t *)tp_ptr)[1] = nsec;
    return 0;
  default:
    return (uint64_t)-22; // EINVAL
  }
}

static uint64_t sys_clock_getres(uint64_t clk_id, uint64_t tp_ptr, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)clk_id;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!tp_ptr)
    return (uint64_t)-14; // EFAULT

  // LAPIC timer has 1ms resolution
  ((uint64_t *)tp_ptr)[0] = 0;
  ((uint64_t *)tp_ptr)[1] = 1000000ULL;

  return 0;
}

// nanosleep(req, rem) - sleep for specified time
// req and rem are pointers to struct timespec { tv_sec, tv_nsec }
static uint64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)rem_ptr;

  if (!req_ptr)
    return (uint64_t)-14; // EFAULT

  uint64_t *req = (uint64_t *)req_ptr;
  uint64_t sec = req[0];
  uint64_t nsec = req[1];

  // Convert to milliseconds
  uint64_t total_ms = sec * 1000 + nsec / 1000000;
  if (total_ms == 0 && nsec > 0)
    total_ms = 1; // Minimum 1ms

  // Busy-wait sleep using LAPIC timer with interrupts enabled
  uint64_t start = lapic_timer_get_ms();
  while ((lapic_timer_get_ms() - start) < total_ms) {
    __asm__ volatile("sti; pause"); // Enable interrupts and pause
  }

  return 0;
}

// gettimeofday(tv, tz) - syscall 97
// tz is ignored in modern Linux; tv contains {tv_sec, tv_usec}
static uint64_t sys_gettimeofday(uint64_t tv_ptr, uint64_t tz_ptr, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)tz_ptr;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!tv_ptr)
    return 0; // Linux allows NULL tv

  uint64_t ms = lapic_timer_get_ms();
  uint64_t sec = ms / 1000;
  uint64_t usec = (ms % 1000) * 1000ULL;

  uint64_t *tv = (uint64_t *)tv_ptr;
  tv[0] = rtc_get_boot_timestamp() + sec; // tv_sec
  tv[1] = usec;                           // tv_usec

  return 0;
}

// mlock(addr, len) - syscall 55 (stub)
// We don't implement memory locking, just return success
static uint64_t sys_mlock(uint64_t addr, uint64_t len, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)addr;
  (void)len;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // Stub: pretend we locked the memory
  return 0;
}

// membarrier(cmd, flags, cpu_id) - syscall 324
// Memory barrier syscall - support all commands for GTK
static uint64_t sys_membarrier(uint64_t cmd, uint64_t flags, uint64_t cpu_id,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)cpu_id;
  (void)a3;
  (void)a4;
  (void)a5;

  // MEMBARRIER_CMD_QUERY (0) - Return supported commands bitmask
  if (cmd == 0) {
    // Return bitmask of all supported commands
    return 0xFF; // Support everything
  }

  // For all other commands, just return success (0)
  // This includes:
  // - MEMBARRIER_CMD_GLOBAL (1)
  // - MEMBARRIER_CMD_GLOBAL_EXPEDITED (2)
  // - MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED (3)
  // - MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED (4)
  // - MEMBARRIER_CMD_PRIVATE_EXPEDITED (5)
  // - Any other future commands
  return 0;
}

void syscall_register_arch(void) {
  syscall_register(SYS_ARCH_PRCTL, sys_arch_prctl);
  syscall_register(SYS_CLOCK_GETTIME, sys_clock_gettime);
  syscall_register(SYS_CLOCK_GETRES, sys_clock_getres);
  syscall_register(SYS_NANOSLEEP, sys_nanosleep);
  syscall_register(SYS_GETTIMEOFDAY, sys_gettimeofday);
  syscall_register(SYS_MLOCK, sys_mlock);
  syscall_register(SYS_MEMBARRIER, sys_membarrier);
}
