/*
 * proc_bench.c — AvoryOS process/thread/futex performance benchmark
 *
 * Measures the exact primitives that Minecraft Alpha hammers:
 *   1. fork + wait4 roundtrip latency (process creation)
 *   2. clone (CLONE_THREAD) + futex CLEARTID join latency (thread creation)
 *   3. Futex uncontended wake/wait roundtrip (mutex fast-path)
 *   4. Futex contended ping-pong between two threads (lock hand-off)
 *   5. Rapid sched_yield cost (voluntary preemption)
 *   6. wait4 WNOHANG polling (non-blocking reap scan)
 *   7. Zombie reap latency (time from child exit to parent reaping)
 *   8. Multi-thread fan-out: spawn N threads, join all (thread pool init cost)
 *
 * Every section reports:
 *   iterations, total µs, avg ns/op, min ns/op, max ns/op, ops/sec
 *
 * Usage:  proc_bench [iterations]          (default 200)
 *
 * Build: x86_64-linux-musl-gcc -static -O2 -Wall -Wextra -fno-stack-protector
 *          userland/proc_bench.c userland/proc_bench_trampoline.S
 *          -o userland/proc_bench.elf
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <linux/futex.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Platform shims — keep this compiling against a bare musl sysroot that may
 * not expose every linux/futex.h constant.
 * ---------------------------------------------------------------------- */
#ifndef FUTEX_WAIT
#define FUTEX_WAIT         0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE         1
#endif
#ifndef FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG 128
#endif
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

#ifndef CLONE_VM
#define CLONE_VM        0x00000100
#endif
#ifndef CLONE_FS
#define CLONE_FS        0x00000200
#endif
#ifndef CLONE_FILES
#define CLONE_FILES     0x00000400
#endif
#ifndef CLONE_SIGHAND
#define CLONE_SIGHAND   0x00000800
#endif
#ifndef CLONE_THREAD
#define CLONE_THREAD    0x00010000
#endif
#ifndef CLONE_SYSVSEM
#define CLONE_SYSVSEM   0x00040000
#endif
#ifndef CLONE_CHILD_SETTID
#define CLONE_CHILD_SETTID  0x01000000
#endif
#ifndef CLONE_CHILD_CLEARTID
#define CLONE_CHILD_CLEARTID 0x00200000
#endif

/* -------------------------------------------------------------------------
 * Raw clone trampoline (proc_bench_trampoline.S)
 * ---------------------------------------------------------------------- */
extern long proc_bench_clone(int (*fn)(void *), void *stack,
                              unsigned long flags, void *arg,
                              int *child_tid);

/* -------------------------------------------------------------------------
 * Timing helpers
 * ---------------------------------------------------------------------- */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------------
 * Futex wrappers
 * ---------------------------------------------------------------------- */
static inline long futex_wait_priv(_Atomic uint32_t *addr, uint32_t val,
                                   const struct timespec *tmo)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, tmo, NULL, 0);
}

static inline long futex_wake_priv(_Atomic uint32_t *addr, int n)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, (uint32_t)n, NULL,
                   NULL, 0);
}

/* -------------------------------------------------------------------------
 * Result record
 * ---------------------------------------------------------------------- */
typedef struct {
    const char *section;
    const char *label;
    unsigned    iters;
    uint64_t    total_ns;
    uint64_t    min_ns;
    uint64_t    max_ns;
    int         failures;
} result_t;

#define MAX_RESULTS 64
static result_t results[MAX_RESULTS];
static unsigned nresults;

static void record(const char *section, const char *label,
                   unsigned iters, uint64_t total_ns,
                   uint64_t min_ns, uint64_t max_ns, int failures)
{
    if (nresults >= MAX_RESULTS) return;
    results[nresults++] = (result_t){
        .section  = section,
        .label    = label,
        .iters    = iters,
        .total_ns = total_ns,
        .min_ns   = min_ns,
        .max_ns   = max_ns,
        .failures = failures,
    };
}

static void print_results(void)
{
    /* Column widths */
    const int W_SEC   = 34;
    const int W_LAB   = 38;
    const int W_ITER  =  7;
    const int W_TOT   = 11;
    const int W_AVG   = 11;
    const int W_MIN   = 10;
    const int W_MAX   = 10;
    const int W_OPS   = 14;
    const int W_FAIL  =  5;

    /* Ruler */
    printf("\n");
    for (int i = 0; i < W_SEC+W_LAB+W_ITER+W_TOT+W_AVG+W_MIN+W_MAX+W_OPS+W_FAIL+10; i++)
        putchar('=');
    printf("\n");

    printf("%-*s  %-*s  %*s  %*s  %*s  %*s  %*s  %*s  %*s\n",
           W_SEC,  "Section",
           W_LAB,  "Scenario",
           W_ITER, "iters",
           W_TOT,  "total(µs)",
           W_AVG,  "avg(ns)",
           W_MIN,  "min(ns)",
           W_MAX,  "max(ns)",
           W_OPS,  "ops/sec",
           W_FAIL, "fail");

    for (int i = 0; i < W_SEC+W_LAB+W_ITER+W_TOT+W_AVG+W_MIN+W_MAX+W_OPS+W_FAIL+10; i++)
        putchar('-');
    printf("\n");

    const char *prev_sec = NULL;
    for (unsigned i = 0; i < nresults; i++) {
        const result_t *r = &results[i];
        double avg_ns  = (r->iters > 0) ? (double)r->total_ns / r->iters : 0.0;
        double ops_sec = (r->total_ns > 0)
                         ? (double)r->iters * 1e9 / (double)r->total_ns
                         : 0.0;
        double total_us = (double)r->total_ns / 1000.0;

        /* Blank separator between sections */
        if (prev_sec && strcmp(prev_sec, r->section) != 0)
            printf("\n");
        prev_sec = r->section;

        /* Use %llu for uint64_t fields — avoids PRIu64 with width specifiers */
        printf("%-*s  %-*s  %*u  %*.1f  %*.1f  %*llu  %*llu  %*.0f  %*d\n",
               W_SEC,  r->section,
               W_LAB,  r->label,
               W_ITER, r->iters,
               W_TOT,  total_us,
               W_AVG,  avg_ns,
               W_MIN,  (unsigned long long)r->min_ns,
               W_MAX,  (unsigned long long)r->max_ns,
               W_OPS,  ops_sec,
               W_FAIL, r->failures);
    }

    for (int i = 0; i < W_SEC+W_LAB+W_ITER+W_TOT+W_AVG+W_MIN+W_MAX+W_OPS+W_FAIL+10; i++)
        putchar('=');
    printf("\n\n");
}

/* =========================================================================
 * 1. fork + wait4 roundtrip
 *
 * Measures: full address-space copy (fork), child exits immediately,
 * parent reaps via wait4(-1, ..., 0).
 * This is the most expensive path — if Minecraft spawns helper processes
 * this is the dominating cost.
 * ====================================================================== */
static void bench_fork_wait4(unsigned iter)
{
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int failures = 0;

    for (unsigned i = 0; i < iter; i++) {
        uint64_t t0 = now_ns();
        pid_t pid = fork();
        if (pid < 0) {
            failures++;
            continue;
        }
        if (pid == 0) {
            /* child — exit immediately, no stdio flush */
            _exit(0);
        }
        /* parent — block until child is reaped */
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        uint64_t dt = now_ns() - t0;

        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }

    record("1. fork + wait4", "fork→child_exit→wait4", iter, total, mn, mx, failures);
}

/* =========================================================================
 * 2. fork + wait4 with WNOHANG polling until zombie appears
 *
 * Measures: non-blocking reap scan cost.  Minecraft may poll rather than
 * block when managing multiple child processes.
 * ====================================================================== */
static void bench_fork_wnohang(unsigned iter)
{
    uint64_t total_poll_ns = 0;
    uint64_t mn = UINT64_MAX, mx = 0;
    int failures = 0;
    unsigned long total_polls = 0;

    for (unsigned i = 0; i < iter; i++) {
        pid_t pid = fork();
        if (pid < 0) { failures++; continue; }
        if (pid == 0) { _exit(0); }

        /* Spin with WNOHANG until we get the zombie */
        uint64_t t0 = now_ns();
        unsigned polls = 0;
        for (;;) {
            int status;
            pid_t r = waitpid(pid, &status, WNOHANG);
            polls++;
            if (r == pid) break;
            if (r < 0 && errno != EINTR) { failures++; break; }
            /* yield one timeslice so child can run */
            sched_yield();
        }
        uint64_t dt = now_ns() - t0;
        total_poll_ns += dt;
        total_polls += polls;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }

    record("1. fork + wait4", "fork→WNOHANG poll→reap", iter,
           total_poll_ns, mn, mx, failures);

    /* Extra diagnostic: average polls per reap */
    printf("  [fork WNOHANG] avg polls per reap: %.1f\n",
           iter > 0 ? (double)total_polls / iter : 0.0);
}

/* =========================================================================
 * 3. clone (CLONE_THREAD flags) + futex CLEARTID join
 *
 * Measures: thread-style clone (shared VM/files/sighand) and the
 * CLONE_CHILD_CLEARTID + FUTEX_WAIT join that musl pthread_join uses.
 * This is what Java threads ultimately call on this kernel.
 * ====================================================================== */

#define THREAD_STACK_SIZE (64 * 1024)

/* Flags used by musl's pthread_create path */
#define THREAD_FLAGS \
    (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | \
     CLONE_SYSVSEM | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)

static int thread_nop(void *arg)
{
    (void)arg;
    /* Return immediately — we measure the pure spawn+join overhead. */
    return 0;
}

static void bench_clone_futex_join(unsigned iter)
{
    /* Allocate one stack, reuse across iterations (threads are sequential). */
    void *stack = mmap(NULL, THREAD_STACK_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        perror("mmap thread stack");
        return;
    }

    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int failures = 0;

    /* Timeout for the FUTEX_WAIT — prevents an infinite hang if the kernel
     * has a bug in CLEARTID wakeup.  500 ms is generous for a nop thread. */
    const struct timespec tmo = { .tv_sec = 0, .tv_nsec = 500000000L };

    for (unsigned i = 0; i < iter; i++) {
        _Atomic uint32_t ctid = 1; /* kernel will clear to 0 on thread exit */

        uint64_t t0 = now_ns();

        long tid = proc_bench_clone(thread_nop,
                                    (char *)stack + THREAD_STACK_SIZE,
                                    THREAD_FLAGS, NULL, (int *)&ctid);
        if (tid < 0) {
            failures++;
            continue;
        }

        /* Join: wait for CLEARTID to zero ctid */
        for (;;) {
            uint32_t v = atomic_load_explicit(&ctid, memory_order_acquire);
            if (v == 0) break;
            errno = 0;
            long rc = futex_wait_priv(&ctid, v, &tmo);
            if (rc < 0 && errno != EAGAIN && errno != EINTR) {
                if (errno == ETIMEDOUT) failures++;
                break;
            }
        }

        uint64_t dt = now_ns() - t0;
        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }

    munmap(stack, THREAD_STACK_SIZE);
    record("3. clone + futex join", "clone(THREAD)→nop→CLEARTID join",
           iter, total, mn, mx, failures);
}

/* =========================================================================
 * 4. Futex uncontended wake/wait roundtrip (mutex fast-path)
 *
 * Thread A: sets word=1, FUTEX_WAKE(1)
 * Thread B: was waiting with FUTEX_WAIT(word,1), wakes, sets word=2, FUTEX_WAKE(1)
 * Main:     sees word=2
 *
 * Simpler version: just measure FUTEX_WAKE on a word with no waiters — this
 * is the fast-path for an uncontended mutex unlock.  Should be very cheap
 * (just the syscall overhead + hash-table scan that finds nobody).
 * ====================================================================== */
static void bench_futex_wake_nowaiter(unsigned iter)
{
    _Atomic uint32_t word = 0;
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int failures = 0;

    for (unsigned i = 0; i < iter; i++) {
        uint64_t t0 = now_ns();
        long r = futex_wake_priv(&word, 1);
        uint64_t dt = now_ns() - t0;

        if (r < 0) failures++;
        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }

    record("4. futex uncontended", "FUTEX_WAKE_PRIVATE (no waiters)",
           iter, total, mn, mx, failures);
}

/* =========================================================================
 * 4b. Futex contended ping-pong
 *
 * Two threads alternate holding a "lock" word, passing it back and forth
 * ITER times each.  This measures the full contended path:
 *   unlock thread: atomic CAS, FUTEX_WAKE
 *   waiter thread: FUTEX_WAIT, wake, re-acquire
 *
 * word encoding:
 *   0 = unlocked
 *   1 = locked by main
 *   2 = locked by helper
 * ====================================================================== */
typedef struct {
    _Atomic uint32_t  word;      /* 0=free, 1=main's turn, 2=helper's turn  */
    unsigned          iter;
    volatile int      helper_done;
    char _pad[56];               /* keep on its own cache line              */
} pingpong_t;

static int pingpong_helper(void *arg)
{
    pingpong_t *pp = (pingpong_t *)arg;
    const struct timespec tmo = { .tv_sec = 1, .tv_nsec = 0 };

    for (unsigned i = 0; i < pp->iter; i++) {
        /* Wait until word == 2 (our turn) */
        for (;;) {
            uint32_t v = atomic_load_explicit(&pp->word, memory_order_acquire);
            if (v == 2) break;
            futex_wait_priv(&pp->word, v, &tmo);
        }
        /* Hand back to main: set word=1 and wake */
        atomic_store_explicit(&pp->word, 1, memory_order_release);
        futex_wake_priv(&pp->word, 1);
    }

    atomic_store_explicit(&pp->helper_done, 1, memory_order_release);
    return 0;
}

static void bench_futex_pingpong(unsigned iter)
{
    void *stack = mmap(NULL, THREAD_STACK_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) { perror("mmap pingpong stack"); return; }

    /* Shared state — must be in accessible memory for both threads. */
    pingpong_t pp;
    memset(&pp, 0, sizeof(pp));
    pp.iter        = iter;
    pp.helper_done = 0;
    atomic_store_explicit(&pp.word, 2, memory_order_relaxed); /* helper goes first */

    _Atomic uint32_t ctid = 1;
    long tid = proc_bench_clone(pingpong_helper,
                                (char *)stack + THREAD_STACK_SIZE,
                                THREAD_FLAGS, &pp, (int *)&ctid);
    if (tid < 0) {
        perror("clone pingpong");
        munmap(stack, THREAD_STACK_SIZE);
        return;
    }

    const struct timespec tmo = { .tv_sec = 1, .tv_nsec = 0 };

    uint64_t t0 = now_ns();
    for (unsigned i = 0; i < iter; i++) {
        /* Wait until word == 1 (our turn) */
        for (;;) {
            uint32_t v = atomic_load_explicit(&pp.word, memory_order_acquire);
            if (v == 1) break;
            futex_wait_priv(&pp.word, v, &tmo);
        }
        /* Hand off to helper */
        atomic_store_explicit(&pp.word, 2, memory_order_release);
        futex_wake_priv(&pp.word, 1);
    }
    uint64_t total = now_ns() - t0;

    /* Wait for helper thread to finish */
    for (;;) {
        uint32_t v = atomic_load_explicit(&ctid, memory_order_acquire);
        if (v == 0) break;
        futex_wait_priv(&ctid, v, &tmo);
    }

    munmap(stack, THREAD_STACK_SIZE);

    /* Each iteration = one full round-trip (main→helper→main) */
    uint64_t per_hop = (iter > 0) ? total / iter : 0;
    record("4b. futex ping-pong", "contended lock hand-off (2 threads)",
           iter, total, per_hop, per_hop, 0);
    printf("  [futex ping-pong] each hop ~%llu ns  (~%.0f hops/sec)\n",
           (unsigned long long)per_hop,
           iter > 0 ? (double)iter * 1e9 / (double)total : 0.0);
}

/* =========================================================================
 * 5. sched_yield cost
 *
 * Voluntary preemption — Java threads call this on contention and in idle
 * spin loops.  High cost here means every Object.wait() / Thread.yield()
 * is expensive.
 * ====================================================================== */
static void bench_sched_yield(unsigned iter)
{
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int failures = 0;

    for (unsigned i = 0; i < iter; i++) {
        uint64_t t0 = now_ns();
        int r = sched_yield();
        uint64_t dt = now_ns() - t0;
        if (r != 0) failures++;
        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }

    record("5. sched_yield", "sched_yield (voluntary preempt)",
           iter, total, mn, mx, failures);
}

/* =========================================================================
 * 6. Thread fan-out: spawn N threads, join all sequentially
 *
 * Models Java thread pool initialization — create a fixed pool of N
 * threads (N = 4, 8, 16) and join them all.  Reports total time and
 * per-thread cost.
 * ====================================================================== */
#define FANOUT_MAX 32

typedef struct {
    _Atomic uint32_t ctid;
} fanout_slot_t;

static int fanout_nop(void *arg) { (void)arg; return 0; }

static void bench_fanout(unsigned n)
{
    if (n > FANOUT_MAX) n = FANOUT_MAX;

    /* Allocate n stacks */
    void *stacks[FANOUT_MAX];
    for (unsigned i = 0; i < n; i++) {
        stacks[i] = mmap(NULL, THREAD_STACK_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (stacks[i] == MAP_FAILED) {
            perror("mmap fanout stack");
            for (unsigned j = 0; j < i; j++) munmap(stacks[j], THREAD_STACK_SIZE);
            return;
        }
    }

    fanout_slot_t slots[FANOUT_MAX];
    const struct timespec tmo = { .tv_sec = 1, .tv_nsec = 0 };

    uint64_t t0 = now_ns();

    /* Spawn all */
    for (unsigned i = 0; i < n; i++) {
        atomic_store_explicit(&slots[i].ctid, 1, memory_order_relaxed);
        long tid = proc_bench_clone(fanout_nop,
                                    (char *)stacks[i] + THREAD_STACK_SIZE,
                                    THREAD_FLAGS, NULL,
                                    (int *)&slots[i].ctid);
        if (tid < 0) {
            /* mark as already done to skip join */
            atomic_store_explicit(&slots[i].ctid, 0, memory_order_relaxed);
        }
    }

    /* Join all */
    for (unsigned i = 0; i < n; i++) {
        for (;;) {
            uint32_t v = atomic_load_explicit(&slots[i].ctid,
                                              memory_order_acquire);
            if (v == 0) break;
            futex_wait_priv(&slots[i].ctid, v, &tmo);
        }
    }

    uint64_t dt = now_ns() - t0;

    for (unsigned i = 0; i < n; i++) munmap(stacks[i], THREAD_STACK_SIZE);

    char label[64];
    snprintf(label, sizeof(label), "spawn+join %u threads (total)", n);
    record("6. thread fan-out", label, 1, dt, dt, dt, 0);

    printf("  [fan-out n=%u] total %llu µs  per-thread %llu µs\n",
           n,
           (unsigned long long)(dt / 1000),
           (unsigned long long)(dt / 1000 / (n ? n : 1)));
}

/* =========================================================================
 * 7. Zombie reap latency — how long from child _exit(0) to parent reaping
 *
 * Fork, child signals via a shared futex (FUTEX_WAKE, not PRIVATE) that it
 * is about to exit, parent starts the timer, child exits, parent blocks in
 * wait4.  The timer measures the kernel's zombie-to-reap path including
 * sched_queue_reap_and_wait.
 *
 * NOTE: must use the non-PRIVATE futex operations here.  After fork() the
 * child has its own mm_struct, so FUTEX_WAIT/WAKE_PRIVATE would key on
 * (parent_mm, addr) vs (child_mm, addr) — they never match and the parent
 * would hang.  FUTEX_WAIT/WAKE (shared) keys on the physical address and
 * works correctly across the fork boundary.
 * ====================================================================== */
typedef struct {
    _Atomic uint32_t ready;  /* child stores 1 just before _exit() */
    char _pad[60];
} zombie_sync_t;

/* Shared (non-PRIVATE) futex wrappers for cross-process signaling. */
static inline long futex_wait_shared(_Atomic uint32_t *addr, uint32_t val,
                                     const struct timespec *tmo)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT, val, tmo, NULL, 0);
}

static inline long futex_wake_shared(_Atomic uint32_t *addr, int n)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE, (uint32_t)n, NULL, NULL, 0);
}

static void bench_zombie_reap(unsigned iter)
{
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int failures = 0;
    const struct timespec tmo = { .tv_sec = 1, .tv_nsec = 0 };

    for (unsigned i = 0; i < iter; i++) {
        /*
         * MAP_SHARED | MAP_ANONYMOUS gives parent and child a mapping backed
         * by the same physical pages, which is required for shared futexes
         * (the kernel keys on the physical address).
         */
        zombie_sync_t *sync = mmap(NULL, sizeof(zombie_sync_t),
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (sync == MAP_FAILED) { failures++; continue; }
        atomic_store_explicit(&sync->ready, 0, memory_order_relaxed);

        pid_t pid = fork();
        if (pid < 0) { munmap(sync, sizeof(*sync)); failures++; continue; }

        if (pid == 0) {
            /*
             * Child: publish ready=1 then wake parent via shared futex.
             * Even if the wake fires before the parent blocks, the parent
             * will see ready==1 on its next load and skip the wait.
             */
            atomic_store_explicit(&sync->ready, 1, memory_order_release);
            futex_wake_shared(&sync->ready, 1);
            _exit(0);
        }

        /* Parent: spin-check then futex-wait until child signals ready. */
        for (;;) {
            uint32_t v = atomic_load_explicit(&sync->ready, memory_order_acquire);
            if (v == 1) break;
            /* ready is still 0, sleep until child writes 1 */
            futex_wait_shared(&sync->ready, 0, &tmo);
        }

        /*
         * Child has set ready=1 and is about to call _exit().  Start the
         * clock now — this measures the time from "child committed to exit"
         * to "parent has fully reaped the zombie".
         */
        uint64_t t0 = now_ns();
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        uint64_t dt = now_ns() - t0;

        munmap(sync, sizeof(*sync));

        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }

    record("7. zombie reap", "fork→signal→_exit→wait4 (reap latency)",
           iter, total, mn, mx, failures);
}

/* =========================================================================
 * 8. Raw syscall overhead baseline
 *
 * getpid() is nearly a no-op inside the kernel.  Its cost reveals the
 * pure syscall entry/exit overhead (register save, privilege switch,
 * handler dispatch, return).  Everything else is measured on top of this.
 * ====================================================================== */
static void bench_syscall_baseline(unsigned iter)
{
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;

    for (unsigned i = 0; i < iter; i++) {
        uint64_t t0 = now_ns();
        syscall(SYS_getpid);
        uint64_t dt = now_ns() - t0;
        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }

    record("8. syscall baseline", "getpid (pure syscall overhead)",
           iter, total, mn, mx, 0);
}

/* =========================================================================
 * 9. wait4 on no-children (ECHILD fast-path)
 *
 * How fast does wait4 return ECHILD?  If the kernel has to walk a long
 * list before deciding there are no children, this will be slow.
 * ====================================================================== */
static void bench_wait4_echild(unsigned iter)
{
    uint64_t total = 0, mn = UINT64_MAX, mx = 0;
    int failures = 0;

    for (unsigned i = 0; i < iter; i++) {
        uint64_t t0 = now_ns();
        pid_t r = waitpid(-1, NULL, WNOHANG);
        uint64_t dt = now_ns() - t0;
        /* expect ECHILD (r==-1, errno==ECHILD) or 0 */
        if (r > 0) failures++;
        total += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }

    record("9. wait4 ECHILD", "wait4(-1, WNOHANG) no children",
           iter, total, mn, mx, failures);
}

/* =========================================================================
 * main
 * ====================================================================== */
int main(int argc, char **argv)
{
    unsigned iter = 200;
    if (argc > 1) {
        char *end;
        unsigned long v = strtoul(argv[1], &end, 10);
        if (*end != '\0' || v == 0 || v > 100000) {
            fprintf(stderr, "usage: %s [iterations: 1..100000]\n", argv[0]);
            return 2;
        }
        iter = (unsigned)v;
    }

    printf("\n");
    printf("=============================================================\n");
    printf("  AvoryOS proc/thread/futex benchmark\n");
    printf("  iterations: %u\n", iter);
    printf("=============================================================\n\n");

    /* Warm up the clock_gettime path */
    for (int i = 0; i < 100; i++) now_ns();

    /*
     * Run order: cheapest first so the system is fully warmed up by the
     * time we hit the expensive fork tests.
     */
    printf("[*] 8. syscall baseline (getpid) ...\n");
    bench_syscall_baseline(iter * 5);  /* more iters for stable average */

    printf("[*] 5. sched_yield ...\n");
    bench_sched_yield(iter);

    printf("[*] 4a. futex WAKE no-waiter ...\n");
    bench_futex_wake_nowaiter(iter * 5);

    printf("[*] 9. wait4 ECHILD fast-path ...\n");
    bench_wait4_echild(iter * 5);

    printf("[*] 3. clone (THREAD) + futex join ...\n");
    bench_clone_futex_join(iter);

    printf("[*] 4b. futex ping-pong (2 threads, %u hops) ...\n", iter);
    bench_futex_pingpong(iter);

    printf("[*] 6. thread fan-out n=4 ...\n");
    bench_fanout(4);
    printf("[*] 6. thread fan-out n=8 ...\n");
    bench_fanout(8);
    printf("[*] 6. thread fan-out n=16 ...\n");
    bench_fanout(16);

    printf("[*] 7. zombie reap latency ...\n");
    bench_zombie_reap(iter / 2 > 0 ? iter / 2 : 1);

    printf("[*] 1a. fork + wait4 (blocking) ...\n");
    bench_fork_wait4(iter);

    printf("[*] 1b. fork + WNOHANG poll ...\n");
    bench_fork_wnohang(iter / 2 > 0 ? iter / 2 : 1);

    /* ------------------------------------------------------------------
     * Print the full results table
     * ---------------------------------------------------------------- */
    print_results();

    /* ------------------------------------------------------------------
     * Interpretation guide — give concrete hints based on observed values
     * ---------------------------------------------------------------- */
    printf("--- Interpretation guide ---\n\n");

    /* Find specific results by section prefix */
    uint64_t baseline_avg  = 0;
    uint64_t fork_avg      = 0;
    uint64_t clone_avg     = 0;
    uint64_t yield_avg     = 0;
    uint64_t futex_w_avg   = 0;

    for (unsigned i = 0; i < nresults; i++) {
        const result_t *r = &results[i];
        uint64_t avg = r->iters > 0 ? r->total_ns / r->iters : 0;
        if (strncmp(r->section, "8.", 2) == 0) baseline_avg = avg;
        if (strncmp(r->section, "1.", 2) == 0 && fork_avg == 0) fork_avg = avg;
        if (strncmp(r->section, "3.", 2) == 0) clone_avg = avg;
        if (strncmp(r->section, "5.", 2) == 0) yield_avg = avg;
        if (strncmp(r->section, "4. futex", 8) == 0) futex_w_avg = avg;
    }

    printf("Syscall entry overhead:  %6llu ns", (unsigned long long)baseline_avg);
    if (baseline_avg > 5000)
        printf("  !! HIGH — kernel syscall dispatch is slow\n");
    else if (baseline_avg > 2000)
        printf("  ! moderate — room to optimise syscall entry\n");
    else
        printf("  OK\n");

    printf("fork + wait4:            %6llu ns", (unsigned long long)fork_avg);
    if (fork_avg > 5000000)
        printf("  !! VERY SLOW — VMA copy / PML4 clone taking too long\n");
    else if (fork_avg > 1000000)
        printf("  ! slow — check vmm_create_pml4 / vma copy path\n");
    else if (fork_avg > 200000)
        printf("  moderate\n");
    else
        printf("  OK\n");

    printf("clone (thread) join:     %6llu ns", (unsigned long long)clone_avg);
    if (clone_avg > 500000)
        printf("  !! VERY SLOW — thread creation is the bottleneck\n");
    else if (clone_avg > 100000)
        printf("  ! slow — check sched_spawn / kernel stack alloc path\n");
    else if (clone_avg > 30000)
        printf("  moderate\n");
    else
        printf("  OK\n");

    printf("futex wake (no waiter):  %6llu ns", (unsigned long long)futex_w_avg);
    if (futex_w_avg > 20000)
        printf("  !! VERY SLOW — futex hash table or spinlock is contended\n");
    else if (futex_w_avg > 5000)
        printf("  ! slow — check futex bucket lock hold time\n");
    else
        printf("  OK\n");

    printf("sched_yield:             %6llu ns", (unsigned long long)yield_avg);
    if (yield_avg > 50000)
        printf("  !! SLOW — scheduler overhead is high per yield\n");
    else if (yield_avg > 10000)
        printf("  ! moderate\n");
    else
        printf("  OK\n");

    printf("\nFor Minecraft Alpha specifically:\n");
    printf("  - Thread creation (clone) dominates workload startup.\n");
    printf("    Target < 100 µs per thread spawn.\n");
    printf("  - Futex ping-pong determines synchronized block throughput.\n");
    printf("    Target < 10 µs per lock hand-off.\n");
    printf("  - If fork_avg >> clone_avg, avoid fork() in hot paths;\n");
    printf("    use threads instead.\n");
    printf("  - If yield_avg > 10 µs, spin loops in Java (unsafe CAS\n");
    printf("    retry) will burn disproportionate CPU time.\n\n");

    return 0;
}
