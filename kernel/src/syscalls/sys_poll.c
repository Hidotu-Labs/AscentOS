// ── Poll/Select Syscalls: poll, ppoll, select, pselect6 ─────────────────────
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "syscall.h"
#include <stdint.h>

// User-space pointer validation: reject kernel/HHDM addresses
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL
static inline bool is_user_ptr(uint64_t addr) {
  return addr != 0 && addr <= USER_ADDR_MAX;
}

struct pollfd {
  int fd;
  short events;
  short revents;
};

static uint64_t do_poll(struct pollfd *fds, uint64_t nfds,
                        uint64_t timeout_ms) {
  struct thread *t = sched_get_current();
  if (t) {
    klog_puts("[POLL] ENTER tid=");
    klog_uint64(t->tid);
    klog_puts(" nfds=");
    klog_uint64(nfds);
    klog_puts(" timeout=");
    klog_uint64(timeout_ms);
    klog_puts("\n");
  }
  if (!t)
    return (uint64_t)-1;

  int ready = 0;
  for (uint64_t i = 0; i < nfds; i++) {
    int fd = fds[i].fd;
    if (fd < 0) {
      fds[i].revents = 0;
      continue;
    }
    if (fd >= MAX_FDS || !t->fds[fd]) {
      fds[i].revents = POLLNVAL;
      ready++;
      continue;
    }
    int ret = vfs_poll(t->fds[fd], fds[i].events);
    if (ret < 0) {
      fds[i].revents = POLLNVAL;
      ready++;
    } else if (ret > 0) {
      fds[i].revents = (short)ret;
      ready++;
    } else {
      fds[i].revents = 0;
    }
  }

  if (ready == 0 && timeout_ms != 0) {
    // Create wait queue entries for each fd we're polling
    // Each fd needs its own entry to avoid list corruption
    wait_queue_entry_t entries[128]; // Max 128 fds per poll
    int entry_fds[128];              // Track which fd each entry belongs to
    int entry_count = 0;

    // Add to all fd wait queues so we get woken when any has events
    for (uint64_t i = 0; i < nfds && entry_count < 128; i++) {
      int fd = fds[i].fd;
      if (fd >= 0 && fd < MAX_FDS && t->fds[fd] && t->fds[fd]->wait_queue) {
        entries[entry_count].thread = t;
        entries[entry_count].next = NULL;
        entry_fds[entry_count] = fd;
        wait_queue_add((wait_queue_t *)t->fds[fd]->wait_queue,
                       &entries[entry_count]);
        entry_count++;
      }
    }

    // Set up timeout if specified
    if (timeout_ms != (uint64_t)-1) {
      t->wakeup_ticks = lapic_timer_get_ticks() + timeout_ms;
    } else {
      t->wakeup_ticks = 0; // No timeout
    }

    // Set state to BLOCKED BEFORE the final check to avoid lost wakeups
    t->state = THREAD_BLOCKED;

    // Final check for ready fds after setting state
    for (uint64_t i = 0; i < nfds; i++) {
      int fd = fds[i].fd;
      if (fd >= 0 && fd < MAX_FDS && t->fds[fd]) {
        if (vfs_poll(t->fds[fd], fds[i].events) > 0) {
          t->state = THREAD_READY;
          ready = 1; // Mark as ready so we don't block
          break;
        }
      }
    }

    // Only yield if we're still blocked
    if (t->state == THREAD_BLOCKED) {
      sched_yield();
    }

    // Remove from all wait queues
    for (int i = 0; i < entry_count; i++) {
      int fd = entry_fds[i];
      if (fd >= 0 && fd < MAX_FDS && t->fds[fd] && t->fds[fd]->wait_queue) {
        wait_queue_remove((wait_queue_t *)t->fds[fd]->wait_queue, &entries[i]);
      }
    }

    // Re-poll to get actual events
    for (uint64_t i = 0; i < nfds; i++) {
      int fd = fds[i].fd;
      if (fd < 0 || fd >= MAX_FDS || !t->fds[fd])
        continue;
      int ret = vfs_poll(t->fds[fd], fds[i].events);
      if (ret > 0) {
        fds[i].revents = (short)ret;
        ready++;
      }
    }
  }
  if (t) {
    klog_puts("[POLL] RETURN tid=");
    klog_uint64(t->tid);
    klog_puts(" ready=");
    klog_uint64((uint64_t)ready);
    klog_puts("\n");
  }
  return (uint64_t)ready;
}

static uint64_t sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (nfds == 0) {
    if (timeout_ms > 0 && timeout_ms != (uint64_t)-1) {
      t->state = THREAD_SLEEPING;
      uint64_t wait = (timeout_ms == (uint64_t)-1) ? 10 : timeout_ms;
      if (wait == 0)
        wait = 1;
      t->wakeup_ticks = lapic_timer_get_ticks() + wait;
      sched_yield();
    }
    return 0;
  }
  if (!is_user_ptr(fds_ptr) ||
      !vmm_is_user_addr_range_valid(fds_ptr, nfds * sizeof(struct pollfd)))
    return (uint64_t)-14;

  return do_poll((struct pollfd *)fds_ptr, nfds, timeout_ms);
}

static uint64_t sys_ppoll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ptr,
                          uint64_t sigmask, uint64_t sigsetsize, uint64_t a5) {
  (void)sigmask;
  (void)sigsetsize;
  (void)a5;

  uint64_t timeout_ms = (uint64_t)-1;
  if (timeout_ptr && is_user_ptr(timeout_ptr)) {
    struct {
      int64_t tv_sec;
      int64_t tv_nsec;
    } *ts = (void *)timeout_ptr;
    if (!vmm_is_user_addr_range_valid(timeout_ptr, 16))
      return (uint64_t)-14;
    timeout_ms = (uint64_t)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
  }

  return sys_poll(fds_ptr, nfds, timeout_ms, 0, 0, 0);
}

static uint64_t do_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                            uint64_t exceptfds, uint64_t timeout_ms) {
  size_t set_size = (nfds + 7) / 8;
  struct pollfd pfds[128];
  uint64_t p_count = 0;
  for (int fd = 0; fd < (int)nfds && p_count < 128; fd++) {
    short events = 0;
    if (readfds && (((uint64_t *)readfds)[fd / 64] & (1ULL << (fd % 64))))
      events |= 0x0001;
    if (writefds && (((uint64_t *)writefds)[fd / 64] & (1ULL << (fd % 64))))
      events |= 0x0004;
    if (events) {
      pfds[p_count].fd = fd;
      pfds[p_count].events = events;
      pfds[p_count].revents = 0;
      p_count++;
    }
  }

  if (p_count == 0) {
    if (timeout_ms != (uint64_t)-1 && timeout_ms > 0) {
      struct thread *t = sched_get_current();
      t->state = THREAD_SLEEPING;
      uint64_t wait = (timeout_ms == (uint64_t)-1) ? 10 : timeout_ms;
      if (wait == 0)
        wait = 1;
      t->wakeup_ticks = lapic_timer_get_ticks() + wait;
      sched_yield();
    }
    if (readfds)
      memset((void *)readfds, 0, set_size);
    if (writefds)
      memset((void *)writefds, 0, set_size);
    if (exceptfds)
      memset((void *)exceptfds, 0, set_size);
    return 0;
  }

  uint64_t ready = do_poll(pfds, p_count, timeout_ms);
  if ((int64_t)ready < 0)
    return ready;

  if (readfds)
    memset((void *)readfds, 0, set_size);
  if (writefds)
    memset((void *)writefds, 0, set_size);
  if (exceptfds)
    memset((void *)exceptfds, 0, set_size);

  uint64_t res_count = 0;
  for (uint64_t i = 0; i < p_count; i++) {
    if (pfds[i].revents == 0)
      continue;
    int fd = pfds[i].fd;
    if (pfds[i].revents & (0x0001 | 0x0010 | 0x0008)) {
      if (readfds)
        ((uint64_t *)readfds)[fd / 64] |= (1ULL << (fd % 64));
      res_count++;
    }
    if (pfds[i].revents & 0x0004) {
      if (writefds)
        ((uint64_t *)writefds)[fd / 64] |= (1ULL << (fd % 64));
      res_count++;
    }
  }
  return res_count;
}

static uint64_t sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                             uint64_t exceptfds, uint64_t timeout,
                             uint64_t sigmask) {
  (void)sigmask;
  if (nfds > 1024)
    return (uint64_t)-22;
  size_t set_size = (nfds + 7) / 8;
  if (readfds && (!is_user_ptr(readfds) ||
                  !vmm_is_user_addr_range_valid(readfds, set_size)))
    return (uint64_t)-14;
  if (writefds && (!is_user_ptr(writefds) ||
                   !vmm_is_user_addr_range_valid(writefds, set_size)))
    return (uint64_t)-14;
  if (exceptfds && (!is_user_ptr(exceptfds) ||
                    !vmm_is_user_addr_range_valid(exceptfds, set_size)))
    return (uint64_t)-14;

  uint64_t timeout_ms = (uint64_t)-1;
  if (timeout && is_user_ptr(timeout)) {
    if (!vmm_is_user_addr_range_valid(timeout, 16))
      return (uint64_t)-14;
    struct {
      int64_t tv_sec;
      int64_t tv_nsec;
    } *ts = (void *)timeout;
    timeout_ms = (uint64_t)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
  }

  return do_pselect6(nfds, readfds, writefds, exceptfds, timeout_ms);
}

static uint64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                           uint64_t exceptfds, uint64_t timeout, uint64_t a5) {
  (void)a5;
  uint64_t timeout_ms = (uint64_t)-1;

  if (timeout && is_user_ptr(timeout)) {
    struct {
      int64_t tv_sec;
      int64_t tv_usec;
    } *tv = (void *)timeout;
    if (!vmm_is_user_addr_range_valid(timeout, 16))
      return (uint64_t)-14;
    timeout_ms = (uint64_t)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
  }

  return do_pselect6(nfds, readfds, writefds, exceptfds, timeout_ms);
}

// ── Register poll/select syscalls ───────────────────────────────────────────
void syscall_register_poll(void) {
  syscall_register(SYS_POLL, sys_poll);
  syscall_register(SYS_PPOLL, sys_ppoll);
  syscall_register(SYS_PSELECT6, sys_pselect6);
  syscall_register(SYS_SELECT, sys_select);

  klog_puts("[OK] Poll/Select syscalls registered\n");
}
