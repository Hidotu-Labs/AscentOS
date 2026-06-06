// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ltp_timerfd.c — Standalone port of LTP timerfd01 / timerfd02 / timerfd04
 *
 * Original authors:
 *   Davide Libenzi <davidel@xmailserver.org>      (timerfd01)
 *   Ulrich Drepper, Andrea Cervesato (SUSE)        (timerfd02)
 *   Cyril Hrubis (SUSE)                            (timerfd04)
 *
 * Ported to standalone musl-static for AscentOS by Antigravity.
 * No LTP framework required: uses only POSIX + Linux syscalls.
 *
 * Tests exercised:
 *  T01 – CLOCK_MONOTONIC relative timer (100 ms), expect 1 tick
 *  T02 – CLOCK_REALTIME  relative timer (100 ms), expect 1 tick
 *  T03 – CLOCK_MONOTONIC absolute timer (100 ms), expect 1 tick
 *  T04 – CLOCK_REALTIME  absolute timer (100 ms), expect 1 tick
 *  T05 – CLOCK_MONOTONIC periodic timer (50 ms × 3 periods, sleep 160 ms)
 *  T06 – CLOCK_REALTIME  periodic timer (50 ms × 3 periods, sleep 160 ms)
 *  T07 – timerfd_create(TFD_NONBLOCK): F_GETFL must include O_NONBLOCK
 *  T08 – timerfd_create(TFD_CLOEXEC):  F_GETFD must include FD_CLOEXEC
 *  T09 – timerfd_create(0):            neither flag should be set
 *  T10 – O_NONBLOCK read on idle timer must return EAGAIN
 *  T11 – timerfd_gettime() returns relative time after abstime set
 *  T12 – Invalid clockid returns EINVAL
 *  T13 – Invalid flags to timerfd_create returns EINVAL
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/timerfd.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                 */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;
static int g_total = 0;

#define PASS(fmt, ...) do {                                             \
    g_total++; g_pass++;                                               \
    printf("  [PASS] " fmt "\n", ##__VA_ARGS__);                       \
} while (0)

#define FAIL(fmt, ...) do {                                             \
    g_total++; g_fail++;                                               \
    printf("  [FAIL] " fmt "\n", ##__VA_ARGS__);                       \
} while (0)

#define FAIL_ERRNO(msg) FAIL("%s: %s", msg, strerror(errno))

static void print_summary(void) {
    printf("\n========================================\n");
    printf("  Results: %d/%d passed", g_pass, g_total);
    if (g_fail)
        printf("  (%d FAILED)", g_fail);
    printf("\n========================================\n");
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static uint64_t clock_us(clockid_t clk) {
    struct timespec ts;
    if (clock_gettime(clk, &ts) < 0) {
        FAIL_ERRNO("clock_gettime");
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/*
 * Set a timerfd and wait for it via poll().
 * Returns the expiry count read, or UINT64_MAX on error.
 */
static uint64_t set_and_wait(int tfd, int flags,
                             uint64_t value_us, uint64_t interval_us) {
    struct itimerspec its = {
        .it_value    = { .tv_sec = value_us / 1000000,
                         .tv_nsec = (value_us % 1000000) * 1000 },
        .it_interval = { .tv_sec = interval_us / 1000000,
                         .tv_nsec = (interval_us % 1000000) * 1000 },
    };
    if (timerfd_settime(tfd, flags, &its, NULL) < 0) {
        FAIL_ERRNO("timerfd_settime");
        return UINT64_MAX;
    }
    struct pollfd pfd = { .fd = tfd, .events = POLLIN };
    if (poll(&pfd, 1, 2000) <= 0) {   /* 2 s timeout */
        FAIL("poll() timed out or failed");
        return UINT64_MAX;
    }
    if (!(pfd.revents & POLLIN)) {
        FAIL("poll() returned but no POLLIN");
        return UINT64_MAX;
    }
    uint64_t ticks = 0;
    if (read(tfd, &ticks, sizeof(ticks)) != (ssize_t)sizeof(ticks)) {
        FAIL_ERRNO("read(timerfd)");
        return UINT64_MAX;
    }
    return ticks;
}

/* ------------------------------------------------------------------ */
/* T01/T02 – relative timer                                             */
/* ------------------------------------------------------------------ */
static void test_relative_timer(clockid_t clk, const char *name) {
    printf("\n[T] %s: relative timer (100 ms, expect 1 tick)\n", name);
    int tfd = timerfd_create(clk, 0);
    if (tfd < 0) { FAIL_ERRNO("timerfd_create"); return; }

    uint64_t ticks = set_and_wait(tfd, 0, 100000, 0);
    if (ticks == 1)
        PASS("got 1 tick");
    else if (ticks != UINT64_MAX)
        FAIL("expected 1 tick, got %llu", (unsigned long long)ticks);

    close(tfd);
}

/* ------------------------------------------------------------------ */
/* T03/T04 – absolute timer                                             */
/* ------------------------------------------------------------------ */
static void test_absolute_timer(clockid_t clk, const char *name) {
    printf("\n[T] %s: absolute timer (now+100 ms, expect 1 tick)\n", name);
    int tfd = timerfd_create(clk, 0);
    if (tfd < 0) { FAIL_ERRNO("timerfd_create"); return; }

    uint64_t tnow = clock_us(clk);
    if (!tnow) { close(tfd); return; }

    uint64_t ticks = set_and_wait(tfd, TFD_TIMER_ABSTIME, tnow + 100000, 0);
    if (ticks == 1)
        PASS("got 1 tick");
    else if (ticks != UINT64_MAX)
        FAIL("expected 1 tick, got %llu", (unsigned long long)ticks);

    close(tfd);
}

/* ------------------------------------------------------------------ */
/* T05/T06 – periodic timer (LTP timerfd01 sequential-timer subtest)   */
/* ------------------------------------------------------------------ */
static void test_periodic_timer(clockid_t clk, const char *name) {
    printf("\n[T] %s: periodic timer (50 ms period, sleep 160 ms → ≥3 ticks)\n",
           name);
    int tfd = timerfd_create(clk, 0);
    if (tfd < 0) { FAIL_ERRNO("timerfd_create"); return; }

    uint64_t tnow = clock_us(clk);
    if (!tnow) { close(tfd); return; }

    /* arm: first fire at now+50ms, then every 50ms.
     * set_and_wait returns UINT64_MAX on error (poll timeout etc.) */
    uint64_t first_tick = set_and_wait(tfd, TFD_TIMER_ABSTIME, tnow + 50000, 50000);
    if (first_tick == UINT64_MAX) {
        /* Timer didn't fire — still check gettime for diagnostic value */
        struct itimerspec cur;
        if (timerfd_gettime(tfd, &cur) == 0) {
            if (cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec <= 50000000LL)
                PASS("timerfd_gettime value is relative and within interval");
            else
                FAIL("timerfd_gettime value looks wrong: %lds %ldns",
                     (long)cur.it_value.tv_sec, (long)cur.it_value.tv_nsec);
        }
        /* Do NOT fall through to the blocking read() */
        close(tfd);
        return;
    }

    /* First tick arrived — check gettime returns relative remaining time */
    struct itimerspec cur;
    if (timerfd_gettime(tfd, &cur) < 0) {
        FAIL_ERRNO("timerfd_gettime");
        close(tfd);
        return;
    }
    /* T11 – value returned by gettime must be relative (< initial interval) */
    if (cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec <= 50000000LL)
        PASS("timerfd_gettime value is relative and within interval");
    else
        FAIL("timerfd_gettime value looks wrong: %lds %ldns",
             (long)cur.it_value.tv_sec, (long)cur.it_value.tv_nsec);

    usleep(160000);  /* sleep 160 ms → should accumulate ≥3 ticks */

    /* Timer is periodic and armed — safe to read (won't block indefinitely) */
    uint64_t ticks = 0;
    if (read(tfd, &ticks, sizeof(ticks)) != (ssize_t)sizeof(ticks)) {
        FAIL_ERRNO("read(timerfd periodic)");
        close(tfd);
        return;
    }
    if (ticks >= 3)
        PASS("periodic: got %llu ticks (≥3 expected)", (unsigned long long)ticks);
    else
        FAIL("periodic: got %llu ticks (expected ≥3)", (unsigned long long)ticks);

    close(tfd);
}

/* ------------------------------------------------------------------ */
/* T07 – TFD_NONBLOCK flag (LTP timerfd02)                             */
/* ------------------------------------------------------------------ */
static void test_flag_nonblock(void) {
    printf("\n[T] timerfd_create(TFD_NONBLOCK): F_GETFL must have O_NONBLOCK\n");
    int fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    if (fd < 0) { FAIL_ERRNO("timerfd_create TFD_NONBLOCK"); return; }

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) { FAIL_ERRNO("F_GETFL"); close(fd); return; }

    if (fl & O_NONBLOCK)
        PASS("F_GETFL has O_NONBLOCK (0x%x)", fl);
    else
        FAIL("F_GETFL missing O_NONBLOCK (got 0x%x)", fl);

    close(fd);
}

/* ------------------------------------------------------------------ */
/* T08 – TFD_CLOEXEC flag (LTP timerfd02)                              */
/* ------------------------------------------------------------------ */
static void test_flag_cloexec(void) {
    printf("\n[T] timerfd_create(TFD_CLOEXEC): F_GETFD must have FD_CLOEXEC\n");
    int fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
    if (fd < 0) { FAIL_ERRNO("timerfd_create TFD_CLOEXEC"); return; }

    int fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0) { FAIL_ERRNO("F_GETFD"); close(fd); return; }

    if (fl & FD_CLOEXEC)
        PASS("F_GETFD has FD_CLOEXEC (0x%x)", fl);
    else
        FAIL("F_GETFD missing FD_CLOEXEC (got 0x%x)", fl);

    close(fd);
}

/* ------------------------------------------------------------------ */
/* T09 – no flags: neither O_NONBLOCK nor FD_CLOEXEC                   */
/* ------------------------------------------------------------------ */
static void test_flag_none(void) {
    printf("\n[T] timerfd_create(0): neither O_NONBLOCK nor FD_CLOEXEC\n");
    int fd = timerfd_create(CLOCK_REALTIME, 0);
    if (fd < 0) { FAIL_ERRNO("timerfd_create 0"); return; }

    int fd_flags = fcntl(fd, F_GETFD, 0);
    int fl_flags = fcntl(fd, F_GETFL, 0);

    if ((fd_flags & FD_CLOEXEC) || (fl_flags & O_NONBLOCK))
        FAIL("unexpected flags set: FD=0x%x FL=0x%x", fd_flags, fl_flags);
    else
        PASS("no spurious flags (FD=0x%x FL=0x%x)", fd_flags, fl_flags);

    close(fd);
}

/* ------------------------------------------------------------------ */
/* T10 – EAGAIN on non-blocking idle timerfd (from LTP timerfd01)      */
/* ------------------------------------------------------------------ */
static void test_nonblock_idle_eagain(void) {
    printf("\n[T] CLOCK_MONOTONIC + O_NONBLOCK: read on idle fd must EAGAIN\n");
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { FAIL_ERRNO("timerfd_create"); return; }

    /* arm, wait once, then re-arm fresh so no ticks are pending */
    uint64_t ticks;
    struct itimerspec arm = {
        .it_value    = { .tv_sec = 0, .tv_nsec = 100000000 }, /* 100 ms */
        .it_interval = { 0 },
    };
    timerfd_settime(tfd, 0, &arm, NULL);
    /* wait for that tick */
    struct pollfd pfd = { .fd = tfd, .events = POLLIN };
    poll(&pfd, 1, 2000);
    read(tfd, &ticks, sizeof(ticks)); /* drain it */

    /* now set O_NONBLOCK and try to read from idle (no pending ticks) */
    int fl = fcntl(tfd, F_GETFL, 0);
    fcntl(tfd, F_SETFL, fl | O_NONBLOCK);

    ssize_t ret = read(tfd, &ticks, sizeof(ticks));
    if (ret < 0 && errno == EAGAIN)
        PASS("read() returned EAGAIN on idle fd (correct)");
    else if (ret > 0)
        FAIL("read() returned %zd ticks — should be 0 pending", ret);
    else
        FAIL("read() failed with unexpected errno %d (%s)", errno, strerror(errno));

    close(tfd);
}

/* ------------------------------------------------------------------ */
/* T12 – invalid clockid (LTP error-condition test)                     */
/* ------------------------------------------------------------------ */
static void test_invalid_clockid(void) {
    printf("\n[T] timerfd_create(invalid clockid) must return EINVAL\n");
    int fd = timerfd_create(9999, 0);
    if (fd < 0 && errno == EINVAL)
        PASS("got EINVAL for bad clockid");
    else if (fd >= 0) {
        FAIL("timerfd_create accepted invalid clockid");
        close(fd);
    } else
        FAIL("got errno %d (%s), expected EINVAL", errno, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* T13 – invalid flags (LTP error-condition test)                       */
/* ------------------------------------------------------------------ */
static void test_invalid_flags(void) {
    printf("\n[T] timerfd_create(invalid flags) must return EINVAL\n");
    int fd = timerfd_create(CLOCK_MONOTONIC, 0xFFFF);
    if (fd < 0 && errno == EINVAL)
        PASS("got EINVAL for bad flags");
    else if (fd >= 0) {
        FAIL("timerfd_create accepted invalid flags");
        close(fd);
    } else
        FAIL("got errno %d (%s), expected EINVAL", errno, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  LTP timerfd standalone test suite\n");
    printf("  (ported for AscentOS / musl-static)\n");
    printf("========================================\n");

    /* --- LTP timerfd01 equivalents --- */
    test_relative_timer(CLOCK_MONOTONIC, "T01 CLOCK_MONOTONIC");
    test_relative_timer(CLOCK_REALTIME,  "T02 CLOCK_REALTIME");
    test_absolute_timer(CLOCK_MONOTONIC, "T03 CLOCK_MONOTONIC");
    test_absolute_timer(CLOCK_REALTIME,  "T04 CLOCK_REALTIME");
    test_periodic_timer(CLOCK_MONOTONIC, "T05/T11 CLOCK_MONOTONIC periodic");
    test_periodic_timer(CLOCK_REALTIME,  "T06/T11 CLOCK_REALTIME periodic");

    /* --- LTP timerfd02 equivalents --- */
    test_flag_nonblock();
    test_flag_cloexec();
    test_flag_none();

    /* --- LTP timerfd01 O_NONBLOCK/EAGAIN subtest --- */
    test_nonblock_idle_eagain();

    /* --- Error-condition tests --- */
    test_invalid_clockid();
    test_invalid_flags();

    print_summary();
    return g_fail ? 1 : 0;
}
