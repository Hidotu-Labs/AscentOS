#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define STACK_SIZE (64 * 1024)
#define DEFAULT_ITERATIONS 1000

extern long ascent_clone(int (*fn)(void *), void *stack,
                         unsigned long flags, void *arg, int *ctid);

static long futex_wait(_Atomic uint32_t *word, uint32_t value,
                       const struct timespec *timeout) {
  return syscall(SYS_futex, word, FUTEX_WAIT, value, timeout, NULL, 0);
}

static _Atomic uint32_t completed;

static int child_main(void *arg) {
  (void)arg;
  atomic_fetch_add_explicit(&completed, 1, memory_order_release);
  return 0;
}

int main(int argc, char **argv) {
  unsigned iterations = DEFAULT_ITERATIONS;
  if (argc > 1) {
    char *end;
    unsigned long parsed = strtoul(argv[1], &end, 10);
    if (*end || parsed == 0 || parsed > 1000000) {
      fprintf(stderr, "usage: %s [iterations: 1..1000000]\n", argv[0]);
      return 2;
    }
    iterations = (unsigned)parsed;
  }

  void *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  const unsigned long flags =
      CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
      CLONE_SYSVSEM | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;

  for (unsigned i = 0; i < iterations; i++) {
    _Atomic uint32_t child_tid = 0;
    long tid = ascent_clone(child_main, (char *)stack + STACK_SIZE, flags, NULL,
                            (int *)&child_tid);
    if (tid < 0) {
      perror("clone");
      return 1;
    }

    /* Join only through CLONE_CHILD_CLEARTID. A timeout turns a broken wake
     * into a failed iteration instead of hanging this test forever. */
    for (unsigned tries = 0;; tries++) {
      uint32_t observed =
          atomic_load_explicit(&child_tid, memory_order_acquire);
      if (observed == 0)
        break;
      struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};
      errno = 0;
      long rc = futex_wait(&child_tid, observed, &timeout);
      if (rc < 0 && errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT) {
        perror("futex");
        return 1;
      }
      if (tries == 49) {
        fprintf(stderr, "FAIL: stuck at iteration %u (tid=%u)\n", i, observed);
        return 1;
      }
    }

    if ((i + 1) % 100 == 0 || i + 1 == iterations)
      printf("clone/futex: %u/%u iterations\n", i + 1, iterations);
  }

  uint32_t done = atomic_load_explicit(&completed, memory_order_acquire);
  if (done != iterations) {
    fprintf(stderr, "FAIL: only %u/%u children completed\n", done, iterations);
    return 1;
  }

  munmap(stack, STACK_SIZE);
  puts("clone/futex iteration test PASSED");
  return 0;
}
