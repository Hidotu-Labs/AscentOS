// Test for the priority-inheritance futex ops: FUTEX_LOCK_PI, FUTEX_TRYLOCK_PI
// and FUTEX_UNLOCK_PI.
//
// The bug this exists for: the kernel answered an unknown futex op with
// -EINVAL, and musl probes kernel PI support by calling LOCK_PI exactly once,
// caching the errno it gets and returning it from
// pthread_mutexattr_setprotocol().  libpulse accepts 0 or ENOTSUP and asserts
// on everything else, so every PulseAudio client aborted at startup:
//
//   [FUTEX] Unsupported op: 6
//   Assertion 'r == 0 || r == 95' failed at ../src/pulsecore/mutex-posix.c:57,
//   function pa_mutex_new(). Aborting.
//
// So the first thing checked here is that probe, in the exact shape musl makes
// it.  The rest drive the ops directly, because musl never emits them itself -
// it probes PI support and then serves a PI mutex with ordinary futexes - so a
// glibc binary is the only other thing that would exercise the kernel path.
//
// Expected exit status: 0 when everything passed.

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_futex
#define SYS_futex 202
#endif

#ifndef FUTEX_LOCK_PI
#define FUTEX_LOCK_PI    6
#endif
#ifndef FUTEX_UNLOCK_PI
#define FUTEX_UNLOCK_PI  7
#endif
#ifndef FUTEX_TRYLOCK_PI
#define FUTEX_TRYLOCK_PI 8
#endif
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_LOCK_PI_PRIVATE    (FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_UNLOCK_PI_PRIVATE  (FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_TRYLOCK_PI_PRIVATE (FUTEX_TRYLOCK_PI | FUTEX_PRIVATE_FLAG)

#define FUTEX_TID_MASK   0x3fffffffu
#define FUTEX_OWNER_DIED 0x40000000u
#define FUTEX_WAITERS    0x80000000u

#define ROUNDS 2000

static int failures;

static void check(int ok, const char *what, long value) {
  if (!ok) {
    printf("FAIL: %s (returned %ld)\n", what, value);
    failures++;
  }
}

/* Returns a futex return code (0 or -errno) rather than the libc -1/errno
 * convention, because the kernel's answer is what is under test. */
static long futex_pi(uint32_t op, uint32_t *word, uint32_t val,
                     const struct timespec *timeout) {
  long r = syscall(SYS_futex, word, op, val, timeout, NULL, 0);
  return r < 0 ? -errno : r;
}

static uint32_t self_tid(void) { return (uint32_t)syscall(SYS_gettid); }

static void realtime_after(struct timespec *ts, long ms) {
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

/* ---------------------------------------------------------------- probes -- */

/* The call libpulse makes, and the one musl turns into a LOCK_PI probe. */
static void test_mutexattr_protocol(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  int r = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
  check(r == 0 || r == ENOTSUP, "setprotocol(PTHREAD_PRIO_INHERIT)", r);

  /* A PI mutex has to be usable afterwards either way. */
  pthread_mutex_t m;
  r = pthread_mutex_init(&m, &attr);
  check(r == 0 || r == ENOTSUP, "pthread_mutex_init(PI)", r);
  if (r == 0) {
    check(pthread_mutex_lock(&m) == 0, "pthread_mutex_lock(PI)", 0);
    check(pthread_mutex_unlock(&m) == 0, "pthread_mutex_unlock(PI)", 0);
    pthread_mutex_destroy(&m);
  }
  pthread_mutexattr_destroy(&attr);
}

/* musl's probe itself: a LOCK_PI against an unlocked word takes the lock. */
static void test_lock_pi_probe(void) {
  uint32_t word = 0;
  long r = futex_pi(FUTEX_LOCK_PI_PRIVATE, &word, 0, NULL);
  check(r == 0, "LOCK_PI probe on an unlocked word", r);
  check((word & FUTEX_TID_MASK) == self_tid(), "LOCK_PI probe recorded us as the owner",
        (long)word);
  r = futex_pi(FUTEX_UNLOCK_PI_PRIVATE, &word, 0, NULL);
  check(r == 0, "UNLOCK_PI after the probe", r);
  check(word == 0, "UNLOCK_PI cleared the word", (long)word);
}

/* ---------------------------------------------------------- handover ------ */

struct handoff {
  uint32_t word;          // the PI lock word
  _Atomic int handed_off; // holder is done with the lock
  _Atomic int released;   // taker gave it back
  _Atomic int inside;     // mutual exclusion canary
  _Atomic int errors;
};

static void *taker(void *arg) {
  struct handoff *h = arg;
  uint32_t tid = self_tid();

  for (int round = 0; round < ROUNDS; round++) {
    while (!atomic_load(&h->handed_off))
      sched_yield(); // holder is still inside its critical section

    long r = futex_pi(FUTEX_LOCK_PI_PRIVATE, &h->word, 0, NULL);
    if (r != 0) {
      atomic_fetch_add(&h->errors, 1);
      printf("FAIL: taker LOCK_PI round %d (returned %ld)\n", round, r);
      atomic_store(&h->released, 1);
      return NULL;
    }
    if ((h->word & FUTEX_TID_MASK) != tid) {
      atomic_fetch_add(&h->errors, 1);
      printf("FAIL: taker owns nothing in round %d (word %#x)\n", round,
             h->word);
    }
    if (atomic_exchange(&h->inside, 1)) {
      atomic_fetch_add(&h->errors, 1);
      printf("FAIL: two owners at once in round %d\n", round);
    }
    atomic_store(&h->inside, 0);

    if (futex_pi(FUTEX_UNLOCK_PI_PRIVATE, &h->word, 0, NULL) != 0) {
      atomic_fetch_add(&h->errors, 1);
      printf("FAIL: taker UNLOCK_PI round %d\n", round);
    }
    atomic_store(&h->handed_off, 0);
    atomic_store(&h->released, 1);
  }
  return NULL;
}

/* Lock handover from a holder to a waiter, over and over. */
static void test_handover(void) {
  struct handoff h;
  h.word = 0;
  atomic_store(&h.handed_off, 0);
  atomic_store(&h.released, 0);
  atomic_store(&h.inside, 0);
  atomic_store(&h.errors, 0);

  pthread_t t;
  if (pthread_create(&t, NULL, taker, &h) != 0) {
    printf("FAIL: pthread_create\n");
    failures++;
    return;
  }

  uint32_t tid = self_tid();
  for (int round = 0; round < ROUNDS; round++) {
    uint32_t expected = 0;
    while (!__atomic_compare_exchange_n(&h.word, &expected, tid, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      expected = 0;
      sched_yield();
    }
    if (atomic_exchange(&h.inside, 1)) {
      printf("FAIL: two owners at once in round %d\n", round);
      failures++;
    }
    atomic_store(&h.inside, 0);

    /* Either the kernel finds the waiter queued and hands the lock straight to
     * it, or it clears the word and the waiter picks it up itself.  Both are
     * correct; what must not happen is an error, or the waiter going
     * unmatched, which the round would hang on. */
    atomic_store(&h.released, 0);
    atomic_store(&h.handed_off, 1);
    long r = futex_pi(FUTEX_UNLOCK_PI_PRIVATE, &h.word, 0, NULL);
    if (r != 0) {
      printf("FAIL: holder UNLOCK_PI round %d (returned %ld)\n", round, r);
      failures++;
      break;
    }
    while (!atomic_load(&h.released))
      ;
  }

  pthread_join(t, NULL);
  check(atomic_load(&h.errors) == 0, "handover rounds",
        (long)atomic_load(&h.errors));
  check(h.word == 0, "lock word released at the end", (long)h.word);
}

/* ------------------------------------------------- trylock and timeout ---- */

struct holder {
  uint32_t word;
  _Atomic int held;
  _Atomic int release;
};

static void *holder(void *arg) {
  struct holder *hd = arg;
  if (futex_pi(FUTEX_LOCK_PI_PRIVATE, &hd->word, 0, NULL) != 0)
    return NULL;
  atomic_store(&hd->held, 1);
  while (!atomic_load(&hd->release))
    sched_yield();
  futex_pi(FUTEX_UNLOCK_PI_PRIVATE, &hd->word, 0, NULL);
  return NULL;
}

static void test_trylock_and_timeout(void) {
  struct holder hd;
  hd.word = 0;
  atomic_store(&hd.held, 0);
  atomic_store(&hd.release, 0);

  pthread_t t;
  if (pthread_create(&t, NULL, holder, &hd) != 0) {
    printf("FAIL: pthread_create(holder)\n");
    failures++;
    return;
  }
  while (!atomic_load(&hd.held))
    sched_yield();
  uint32_t holder_tid = hd.word & FUTEX_TID_MASK;

  long r = futex_pi(FUTEX_TRYLOCK_PI_PRIVATE, &hd.word, 0, NULL);
  /* Linux spells a contended PI trylock -EAGAIN (EWOULDBLOCK), and glibc's
   * robust PI trylock path checks for exactly that errno, so this is not
   * interchangeable with -EBUSY. */
  check(r == -EAGAIN, "TRYLOCK_PI on a held lock reports EWOULDBLOCK", r);

  struct timespec to;
  realtime_after(&to, 300);
  r = futex_pi(FUTEX_LOCK_PI_PRIVATE, &hd.word, 0, &to);
  check(r == -ETIMEDOUT, "LOCK_PI on a held lock times out", r);
  check((hd.word & FUTEX_TID_MASK) == holder_tid,
        "a timed out waiter left the owner alone", (long)hd.word);

  /* Unlocking a lock we do not own is not something to be quiet about. */
  uint32_t not_ours = holder_tid + 1;
  r = futex_pi(FUTEX_UNLOCK_PI_PRIVATE, &not_ours, 0, NULL);
  check(r == -EPERM, "UNLOCK_PI by a non-owner reports EPERM", r);

  atomic_store(&hd.release, 1);
  pthread_join(t, NULL);
  check(hd.word == 0, "word clear after the holder released it", (long)hd.word);
}

/* ------------------------------------------- owner that never comes back -- */

static uint32_t leaker_word;
static _Atomic uint32_t leaker_tid;
static _Atomic int leaker_held;

static void *leaker(void *arg) {
  (void)arg;
  if (futex_pi(FUTEX_LOCK_PI_PRIVATE, &leaker_word, 0, NULL) != 0)
    return NULL;
  atomic_store(&leaker_tid, self_tid());
  atomic_store(&leaker_held, 1);
  pthread_exit(NULL); // walks out holding the lock: no unlock, ever
}

/* A thread that dies holding a PI lock would otherwise park every later
 * acquirer forever, because nothing is left to run the unlock. */
static void test_dead_owner(void) {
  leaker_word = 0;
  atomic_store(&leaker_tid, 0);
  atomic_store(&leaker_held, 0);

  pthread_t t;
  if (pthread_create(&t, NULL, leaker, NULL) != 0) {
    printf("FAIL: pthread_create(leaker)\n");
    failures++;
    return;
  }
  pthread_join(t, NULL);
  check(atomic_load(&leaker_held) == 1, "leaker took the lock",
        atomic_load(&leaker_held));
  check((leaker_word & FUTEX_TID_MASK) == atomic_load(&leaker_tid),
        "word names the leaker", (long)leaker_word);

  /* The tid may take a moment to stop resolving while the thread is reaped, so
   * give the kernel a couple of seconds in total to notice.  Linux answers a
   * dead owner that left no robust list with -ESRCH; this kernel recovers the
   * lock instead, so accept either and check the recovery when it happens. */
  long r = -ETIMEDOUT;
  for (int attempt = 0; attempt < 10 && r != 0; attempt++) {
    struct timespec to;
    realtime_after(&to, 200);
    r = futex_pi(FUTEX_LOCK_PI_PRIVATE, &leaker_word, 0, &to);
  }
  check(r == 0 || r == -ESRCH, "a dead owner must not wedge the lock", r);
  if (r == 0) {
    check((leaker_word & FUTEX_OWNER_DIED) != 0,
          "dead owner recorded in the word", (long)leaker_word);
    long u = futex_pi(FUTEX_UNLOCK_PI_PRIVATE, &leaker_word, 0, NULL);
    check(u == 0, "recovered lock unlocks", u);
  } else {
    leaker_word = 0; // -ESRCH leaves the stale word behind; drop it by hand
  }
}

int main(void) {
  printf("futex PI: %d handover rounds\n", ROUNDS);

  test_mutexattr_protocol();
  test_lock_pi_probe();
  test_handover();
  test_trylock_and_timeout();
  test_dead_owner();

  if (failures) {
    printf("futex PI: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("futex PI: all checks passed\n");
  return 0;
}
