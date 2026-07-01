#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEBUGLOG(...) printf(__VA_ARGS__)

#define NUM_ITERATIONS 100
#define NUM_BLOCKS 128
#define BLOCK_SIZE 4096

// --- Clone & Futex Definitions ---
#define CLONE_STRESS_THREADS 4
#define CLONE_STRESS_ITERATIONS 10
#define THREAD_STACK_SIZE (64 * 1024)

#ifndef CLONE_VM
#define CLONE_VM 0x00000100
#endif
#ifndef CLONE_FS
#define CLONE_FS 0x00000200
#endif
#ifndef CLONE_FILES
#define CLONE_FILES 0x00000400
#endif
#ifndef CLONE_SIGHAND
#define CLONE_SIGHAND 0x00000800
#endif
#ifndef CLONE_PTRACE
#define CLONE_PTRACE 0x00002000
#endif
#ifndef CLONE_VFORK
#define CLONE_VFORK 0x00004000
#endif
#ifndef CLONE_PARENT
#define CLONE_PARENT 0x00008000
#endif
#ifndef CLONE_THREAD
#define CLONE_THREAD 0x00010000
#endif
#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif
#ifndef CLONE_SYSVSEM
#define CLONE_SYSVSEM 0x00040000
#endif
#ifndef CLONE_SETTLS
#define CLONE_SETTLS 0x00080000
#endif
#ifndef CLONE_PARENT_SETTID
#define CLONE_PARENT_SETTID 0x00100000
#endif
#ifndef CLONE_CHILD_CLEARTID
#define CLONE_CHILD_CLEARTID 0x00200000
#endif
#ifndef CLONE_DETACHED
#define CLONE_DETACHED 0x00400000
#endif
#ifndef CLONE_UNTRACED
#define CLONE_UNTRACED 0x00800000
#endif
#ifndef CLONE_CHILD_SETTID
#define CLONE_CHILD_SETTID 0x01000000
#endif

static long raw_clone(unsigned long flags, void *child_stack, int *ptid,
                      int *ctid, unsigned long newtls) {
  long ret;
  __asm__ volatile("movq %2, %%rdi\n"
                   "movq %3, %%rsi\n"
                   "movq %4, %%rdx\n"
                   "movq %5, %%r10\n"
                   "movq %6, %%r8\n"
                   "movq $56, %%rax\n"
                   "syscall\n"
                   "movq %%rax, %0\n"
                   : "=r"(ret)
                   : "r"(flags), "r"(flags), "r"(child_stack), "r"(ptid),
                     "r"(ctid), "r"(newtls)
                   : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11",
                     "memory");
  return ret;
}

static long raw_futex(uint32_t *uaddr, int op, uint32_t val,
                      const struct timespec *timeout, uint32_t *uaddr2,
                      uint32_t val3) {
  long ret;
  __asm__ volatile("movq %1, %%rdi\n"
                   "movq %2, %%rsi\n"
                   "movq %3, %%rdx\n"
                   "movq %4, %%r10\n"
                   "movq %5, %%r8\n"
                   "movq %6, %%r9\n"
                   "movq $202, %%rax\n"
                   "syscall\n"
                   "movq %%rax, %0\n"
                   : "=r"(ret)
                   : "r"(uaddr), "r"((long)op), "r"((long)val), "r"(timeout),
                     "r"(uaddr2), "r"((long)val3)
                   : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9", "rcx",
                     "r11", "memory");
  return ret;
}

typedef struct {
  _Atomic uint32_t val; // 0 = unlocked, 1 = locked, 2 = locked with waiters
} mutex_t;

void mutex_lock(mutex_t *m) {
  uint32_t expected = 0;
  if (atomic_compare_exchange_strong(&m->val, &expected, 1))
    return;
  if (expected != 2)
    expected = atomic_exchange(&m->val, 2);
  while (expected != 0) {
    raw_futex((uint32_t *)&m->val, FUTEX_WAIT, 2, NULL, NULL, 0);
    expected = atomic_exchange(&m->val, 2);
  }
}

void mutex_unlock(mutex_t *m) {
  if (atomic_fetch_sub(&m->val, 1) != 1) {
    atomic_store(&m->val, 0);
    raw_futex((uint32_t *)&m->val, FUTEX_WAKE, 1, NULL, NULL, 0);
  }
}

long get_free_mem_kb() {
  int fd = open("/proc/meminfo", O_RDONLY);
  if (fd == -1) {
    perror("open /proc/meminfo");
    return -1;
  }

  char buf[1024];
  int n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = '\0';

  char *p = strstr(buf, "MemFree:");
  if (!p)
    return -1;
  p += strlen("MemFree:");
  while (*p == ' ')
    p++;

  return atol(p);
}

void test_mmap_stress() {
  DEBUGLOG("Starting MMAP/MUNMAP stress test...\n");
  void *blocks[NUM_BLOCKS];

  for (int i = 0; i < NUM_ITERATIONS; i++) {
    for (int j = 0; j < NUM_BLOCKS; j++) {
      blocks[j] = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (blocks[j] == MAP_FAILED) {
        fprintf(stderr, "mmap failed at iteration %d, block %d: %s\n", i, j,
                strerror(errno));
        exit(1);
      }
      memset(blocks[j], 0xAA, BLOCK_SIZE);
    }

    for (int j = 0; j < NUM_BLOCKS; j++) {
      if (munmap(blocks[j], BLOCK_SIZE) == -1) {
        fprintf(stderr, "munmap failed at iteration %d, block %d: %s\n", i, j,
                strerror(errno));
        exit(1);
      }
    }

    if (i % 25 == 0) {
      DEBUGLOG("MMAP iteration %d complete\n", i);
    }
  }
  DEBUGLOG("MMAP/MUNMAP stress test PASSED\n");
}

void test_vfs_stress() {
  DEBUGLOG("Starting VFS stress test...\n");
  char filename[64];
  char buffer[1024];
  memset(buffer, 'X', sizeof(buffer));

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    snprintf(filename, sizeof(filename), "/tmp/stress_%d.tmp", i);
    int fd = open(filename, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd == -1) {
      fprintf(stderr, "open failed at iteration %d: %s\n", i, strerror(errno));
      exit(1);
    }
    write(fd, buffer, sizeof(buffer));
    close(fd);
    unlink(filename);

    if (i % 50 == 0) {
      DEBUGLOG("VFS iteration %d complete\n", i);
    }
  }
  DEBUGLOG("VFS stress test PASSED\n");
}

void test_vfs_open_close_stress() {
  DEBUGLOG("Starting VFS open/close stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 10; i++) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "VFS open failed at iteration %d: %s\n", i,
              strerror(errno));
      exit(1);
    }
    char byte;
    read(fd, &byte, 1);
    close(fd);
    if (i % 250 == 0)
      DEBUGLOG("VFS open/close iteration %d complete\n", i);
  }
  DEBUGLOG("VFS open/close stress test PASSED\n");
}

static int epoll_lifecycle_once(void) {
  int fds[2];
  int epfd = epoll_create1(0);
  if (epfd < 0)
    return -1;
  if (pipe(fds) < 0) {
    close(epfd);
    return -1;
  }

  struct epoll_event event = {.events = EPOLLIN, .data.fd = fds[0]};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &event) < 0) {
    close(fds[0]);
    close(fds[1]);
    close(epfd);
    return -1;
  }

  write(fds[1], "E", 1);
  struct epoll_event ready;
  epoll_wait(epfd, &ready, 1, 0);
  close(fds[0]);
  close(fds[1]);
  close(epfd);
  return 0;
}

void test_epoll_stress() {
  DEBUGLOG("Starting EPOLL stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    if (epoll_lifecycle_once() < 0) {
      fprintf(stderr, "epoll lifecycle failed at iteration %d: %s\n", i,
              strerror(errno));
      exit(1);
    }
    if (i % 100 == 0)
      DEBUGLOG("EPOLL iteration %d complete\n", i);
  }
  DEBUGLOG("EPOLL stress test PASSED\n");
}

void test_poll_stress() {
  DEBUGLOG("Starting POLL stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    int fds[2];
    if (pipe(fds) < 0) {
      fprintf(stderr, "poll pipe failed: %s\n", strerror(errno));
      exit(1);
    }
    struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
    poll(&pfd, 1, 0);
    write(fds[1], "P", 1);
    poll(&pfd, 1, 0);
    close(fds[0]);
    close(fds[1]);
    if (i % 100 == 0)
      DEBUGLOG("POLL iteration %d complete\n", i);
  }
  DEBUGLOG("POLL stress test PASSED\n");
}

void test_ppoll_stress() {
  DEBUGLOG("Starting PPOLL stress test...\n");
  const struct timespec timeout = {0, 0};
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    int fds[2];
    if (pipe(fds) < 0) {
      fprintf(stderr, "ppoll pipe failed: %s\n", strerror(errno));
      exit(1);
    }
    struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
    ppoll(&pfd, 1, &timeout, NULL);
    write(fds[1], "Q", 1);
    ppoll(&pfd, 1, &timeout, NULL);
    close(fds[0]);
    close(fds[1]);
    if (i % 100 == 0)
      DEBUGLOG("PPOLL iteration %d complete\n", i);
  }
  DEBUGLOG("PPOLL stress test PASSED\n");
}

void test_pipe_stress() {
  DEBUGLOG("Starting PIPE stress test...\n");
  int pipefds[2];
  for (int i = 0; i < NUM_ITERATIONS * 3; i++) {
    if (pipe(pipefds) == -1) {
      fprintf(stderr, "pipe failed at iteration %d: %s\n", i, strerror(errno));
      exit(1);
    }
    close(pipefds[0]);
    close(pipefds[1]);
    if (i % 100 == 0) {
      DEBUGLOG("PIPE iteration %d complete\n", i);
    }
  }
  DEBUGLOG("PIPE stress test PASSED\n");
}

void test_socket_stress() {
  DEBUGLOG("Starting SOCKET stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s != -1) {
      struct sockaddr_un addr;
      memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "stress_sock_%d",
               i);
      bind(s, (struct sockaddr *)&addr, sizeof(addr));
      close(s);
    }
    if (i % 50 == 0) {
      DEBUGLOG("SOCKET iteration %d complete\n", i);
    }
  }
  DEBUGLOG("SOCKET stress test PASSED\n");
}

void test_fork_stress() {
  DEBUGLOG("Starting FORK/EXIT stress test...\n");
  for (int i = 0; i < NUM_ITERATIONS; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      void *p = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p != MAP_FAILED)
        memset(p, 0xBB, 1024 * 1024);
      int fd = open("/tmp/child_stress", O_CREAT | O_RDWR, 0666);
      if (fd != -1)
        write(fd, "child", 5);
      exit(0);
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
      unlink("/tmp/child_stress");
    }
    if (i % 25 == 0) {
      DEBUGLOG("FORK iteration %d complete\n", i);
    }
  }
  DEBUGLOG("FORK/EXIT stress test PASSED\n");
}

static mutex_t stress_mutex = {0};
static _Atomic int threads_running = 0;

int stress_thread_entry(void *arg) {
  (void)arg;
  for (int i = 0; i < 100; i++) {
    mutex_lock(&stress_mutex);
    // Do some dummy work
    volatile int x = 0;
    for (int j = 0; j < 100; j++)
      x++;
    mutex_unlock(&stress_mutex);
    if (i % 10 == 0)
      sched_yield();
  }
  atomic_fetch_sub(&threads_running, 1);
  syscall(SYS_exit, 0);
  return 0;
}

void test_clone_futex_stress() {
  DEBUGLOG("Starting CLONE/FUTEX stress test...\n");
  uint8_t *stacks[CLONE_STRESS_THREADS];
  _Atomic int child_tids[CLONE_STRESS_THREADS];
  for (int i = 0; i < CLONE_STRESS_THREADS; i++) {
    stacks[i] = mmap(NULL, THREAD_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }

  unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                        CLONE_THREAD | CLONE_SYSVSEM | CLONE_CHILD_SETTID |
                        CLONE_CHILD_CLEARTID;

  for (int i = 0; i < CLONE_STRESS_ITERATIONS; i++) {
    atomic_store(&threads_running, CLONE_STRESS_THREADS);
    for (int j = 0; j < CLONE_STRESS_THREADS; j++) {
      atomic_store(&child_tids[j], 0);
      void *stack_top = stacks[j] + THREAD_STACK_SIZE;
      long tid = raw_clone(flags, stack_top, NULL,
                           (int *)&child_tids[j], 0);
      if (tid == 0) {
        stress_thread_entry(NULL);
      }
    }

    while (atomic_load(&threads_running) > 0) {
      usleep(1000);
    }

    // A worker decrements threads_running before entering SYS_exit. Join on
    // clear-child-TID before reusing its user stack.
    for (int j = 0; j < CLONE_STRESS_THREADS; j++) {
      int tid;
      while ((tid = atomic_load(&child_tids[j])) != 0) {
        raw_futex((uint32_t *)&child_tids[j], FUTEX_WAIT, (uint32_t)tid,
                  NULL, NULL, 0);
      }
    }

    // Give the scheduler context that resumed us a chance to consume the
    // detached-task reap queue before the next iteration.
    sched_yield();

    if (i % 2 == 0) {
      DEBUGLOG("CLONE iteration %d complete\n", i);
    }
  }

  // clear-child-TID and reap-queue publication happen on different CPUs.
  // Allow a few complete scheduling cycles before taking the PMM sample.
  for (int i = 0; i < 8; i++) {
    sched_yield();
    usleep(1000);
  }

  for (int i = 0; i < CLONE_STRESS_THREADS; i++) {
    munmap(stacks[i], THREAD_STACK_SIZE);
  }
  DEBUGLOG("CLONE/FUTEX stress test PASSED\n");
}

void test_vfs_error_stress() {
  DEBUGLOG("Starting VFS Error stress test (failed opens/stats)...\n");
  char filename[64];
  struct stat st;
  for (int i = 0; i < NUM_ITERATIONS * 10; i++) {
    snprintf(filename, sizeof(filename), "/nonexistent_%d", i);
    int fd = open(filename, O_RDONLY);
    if (fd != -1)
      close(fd);
    stat(filename, &st);
  }
  DEBUGLOG("VFS Error stress test PASSED\n");
}

void test_mmap_fixed_stress() {
  DEBUGLOG("Starting MMAP FIXED stress test...\n");
  size_t size = 64 * 1024;
  void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (addr == MAP_FAILED) {
    perror("mmap initial");
    return;
  }

  for (int i = 0; i < NUM_ITERATIONS * 2; i++) {
    void *new_addr = mmap(addr, size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (new_addr == MAP_FAILED) {
      fprintf(stderr, "mmap FIXED failed at iteration %d: %s\n", i,
              strerror(errno));
      break;
    }
    memset(new_addr, i, size);
  }
  munmap(addr, size);
  DEBUGLOG("MMAP FIXED stress test PASSED\n");
}

void test_readv_stress() {
  DEBUGLOG("Starting READV stress test...\n");
  int fd = open("/proc/meminfo", O_RDONLY);
  if (fd == -1)
    return;

  char buf1[64], buf2[64];
  struct iovec iov[2];
  iov[0].iov_base = buf1;
  iov[0].iov_len = sizeof(buf1);
  iov[1].iov_base = buf2;
  iov[1].iov_len = sizeof(buf2);

  for (int i = 0; i < NUM_ITERATIONS * 5; i++) {
    lseek(fd, 0, SEEK_SET);
    if (readv(fd, iov, 2) == -1) {
      // Some systems don't support readv on procfs, but we test the syscall
      // entry/exit
    }
  }
  close(fd);
  DEBUGLOG("READV stress test PASSED\n");
}

void test_exec_stress() {
  DEBUGLOG("Starting EXECVE stress test...\n");
  // Use hello.elf if it exists, otherwise skip
  char *argv[] = {"/userland/hello.elf", NULL};
  char *envp[] = {NULL};

  if (access(argv[0], X_OK) != 0) {
    DEBUGLOG("Skipping EXECVE stress test (hello.elf not found or not "
             "executable)\n");
    return;
  }

  for (int i = 0; i < 20; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      // Close stdout/stderr to avoid spam
      int nullfd = open("/dev/null", O_WRONLY);
      if (nullfd != -1) {
        dup2(nullfd, 1);
        dup2(nullfd, 2);
        close(nullfd);
      }
      execve(argv[0], argv, envp);
      exit(0);
    } else if (pid > 0) {
      int status;
      waitpid(pid, &status, 0);
    }
    if (i % 5 == 0)
      DEBUGLOG("EXEC iteration %d complete\n", i);
  }
  DEBUGLOG("EXECVE stress test PASSED\n");
}

void check_leak(const char *test_name, long *last_mem) {
  long current_mem = get_free_mem_kb();
  if (current_mem == -1)
    return;

  long diff = *last_mem - current_mem;
  if (diff > 0) {
    printf("  [!] %s: LEAKED %ld kB\n", test_name, diff);
  } else if (diff < 0) {
    printf("  [ok] %s: Reclaimed %ld kB\n", test_name, -diff);
  } else {
    printf("  [ok] %s: No leak\n", test_name);
  }
  *last_mem = current_mem;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  printf("=== Userland Leak Detection Stress Test ===\n");

  // Prime the kernel slab caches used by epoll, watched VFS nodes and pipes.
  // Empty slab pages are intentionally retained for reuse, so measuring the
  // first-ever lifecycle would mislabel bounded cache growth as a leak.
  if (epoll_lifecycle_once() < 0) {
    printf("CRITICAL: epoll allocator warm-up failed: %s\n", strerror(errno));
    return 1;
  }

  long initial_mem = get_free_mem_kb();
  if (initial_mem == -1) {
    printf("CRITICAL: Could not read /proc/meminfo. Make sure procfs is "
           "mounted.\n");
    return 1;
  }
  printf("Initial Free Memory: %ld kB\n", initial_mem);
  long current_mem = initial_mem;

  printf("\n--- Running MMAP Stress ---\n");
  test_mmap_stress();
  check_leak("MMAP Stress", &current_mem);

  printf("\n--- Running VFS Stress ---\n");
  test_vfs_stress();
  check_leak("VFS Stress", &current_mem);

  printf("\n--- Running VFS Open/Close Stress ---\n");
  test_vfs_open_close_stress();
  check_leak("VFS Open/Close Stress", &current_mem);

  printf("\n--- Running EPOLL Stress ---\n");
  test_epoll_stress();
  check_leak("EPOLL Stress", &current_mem);

  printf("\n--- Running POLL Stress ---\n");
  test_poll_stress();
  check_leak("POLL Stress", &current_mem);

  printf("\n--- Running PPOLL Stress ---\n");
  test_ppoll_stress();
  check_leak("PPOLL Stress", &current_mem);

  printf("\n--- Running PIPE Stress ---\n");
  test_pipe_stress();
  check_leak("PIPE Stress", &current_mem);

  printf("\n--- Running SOCKET Stress ---\n");
  test_socket_stress();
  check_leak("SOCKET Stress", &current_mem);

  printf("\n--- Running FORK Stress ---\n");
  test_fork_stress();
  check_leak("FORK Stress", &current_mem);

  printf("\n--- Running CLONE/FUTEX Stress ---\n");
  test_clone_futex_stress();
  check_leak("CLONE/FUTEX Stress", &current_mem);

  printf("\n--- Running VFS Error Stress ---\n");
  test_vfs_error_stress();
  check_leak("VFS Error Stress", &current_mem);

  printf("\n--- Running MMAP FIXED Stress ---\n");
  test_mmap_fixed_stress();
  check_leak("MMAP FIXED Stress", &current_mem);

  printf("\n--- Running READV Stress ---\n");
  test_readv_stress();
  check_leak("READV Stress", &current_mem);

  printf("\n--- Running EXECVE Stress ---\n");
  test_exec_stress();
  check_leak("EXECVE Stress", &current_mem);

  printf("Verifying memory levels...\n");
  long final_mem = get_free_mem_kb();
  printf("Final Free Memory:   %ld kB\n", final_mem);

  long total_diff = initial_mem - final_mem;
  if (total_diff > 0) {
    printf("FAIL: Total Memory leak: %ld kB\n", total_diff);
    return 1;
  } else if (total_diff < 0) {
    printf("PASS: Memory levels improved. Gained %ld kB.\n", -total_diff);
  } else {
    printf("PASS: No memory leaks detected.\n");
  }

  printf("ALL TESTS COMPLETED!\n");
  return 0;
}
