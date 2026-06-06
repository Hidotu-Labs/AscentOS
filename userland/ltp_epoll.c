// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ltp_epoll.c — Standalone port of LTP epoll_create01 / epoll_ctl01 / epoll_wait01
 *
 * Original authors:
 *   Davide Libenzi <davidel@xmailserver.org>
 *   Ulrich Drepper <drepper@redhat.com>
 *
 * Ported to standalone musl-static for AscentOS by Antigravity.
 * No LTP framework required: uses only POSIX + Linux syscalls.
 *
 * Tests exercised:
 *  T01 – epoll_create1(0) returns valid FD
 *  T02 – epoll_create1(EPOLL_CLOEXEC) sets FD_CLOEXEC
 *  T03 – epoll_create1(invalid flags) returns EINVAL
 *  T04 – epoll_ctl(ADD) with pipe fds
 *  T05 – epoll_ctl(ADD) duplicate fd returns EEXIST
 *  T06 – epoll_ctl(MOD) modifies events
 *  T07 – epoll_ctl(DEL) removes fd
 *  T08 – epoll_ctl(DEL) non-existent fd returns ENOENT
 *  T09 – epoll_wait: timeout returns 0
 *  T10 – epoll_wait: detects EPOLLOUT on empty pipe write-end
 *  T11 – epoll_wait: detects EPOLLIN after write to pipe
 *  T12 – epoll_wait: detects both EPOLLIN|EPOLLOUT on partial pipe
 *  T13 – Nested epoll: adding one epoll FD to another
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/poll.h>

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
/* Creation tests (epoll_create01)                                      */
/* ------------------------------------------------------------------ */

static void test_create(void) {
    printf("\n[T] epoll_create1(0): basic creation\n");
    int epfd = epoll_create1(0);
    if (epfd >= 0) {
        PASS("created epoll fd %d", epfd);
        close(epfd);
    } else {
        FAIL_ERRNO("epoll_create1(0)");
    }

    printf("\n[T] epoll_create1(EPOLL_CLOEXEC): flag check\n");
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd >= 0) {
        int flags = fcntl(epfd, F_GETFD);
        if (flags >= 0 && (flags & FD_CLOEXEC))
            PASS("FD_CLOEXEC is set");
        else
            FAIL("FD_CLOEXEC not set (flags=0x%x)", flags);
        close(epfd);
    } else {
        FAIL_ERRNO("epoll_create1(EPOLL_CLOEXEC)");
    }

    printf("\n[T] epoll_create1(invalid flags): EINVAL check\n");
    epfd = epoll_create1(0xFFFF);
    if (epfd < 0 && errno == EINVAL)
        PASS("got EINVAL as expected");
    else if (epfd >= 0) {
        FAIL("accepted invalid flags");
        close(epfd);
    } else {
        FAIL_ERRNO("epoll_create1(0xFFFF)");
    }
}

/* ------------------------------------------------------------------ */
/* Control tests (epoll_ctl01)                                          */
/* ------------------------------------------------------------------ */

static void test_ctl(void) {
    printf("\n[T] epoll_ctl: ADD, MOD, DEL functionality\n");
    int epfd = epoll_create1(0);
    if (epfd < 0) { FAIL_ERRNO("epoll_create1"); return; }

    int p[2];
    if (pipe(p) < 0) { FAIL_ERRNO("pipe"); close(epfd); return; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = p[0];

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev) == 0)
        PASS("ADD read-end success");
    else
        FAIL_ERRNO("ADD read-end");

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev) < 0 && errno == EEXIST)
        PASS("ADD duplicate returns EEXIST");
    else
        FAIL("ADD duplicate: expected EEXIST, got %d (%s)", errno, strerror(errno));

    ev.events = EPOLLIN | EPOLLOUT;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, p[0], &ev) == 0)
        PASS("MOD success");
    else
        FAIL_ERRNO("MOD");

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, p[0], NULL) == 0)
        PASS("DEL success");
    else
        FAIL_ERRNO("DEL");

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, p[0], NULL) < 0 && errno == ENOENT)
        PASS("DEL non-existent returns ENOENT");
    else
        FAIL("DEL non-existent: expected ENOENT, got %d (%s)", errno, strerror(errno));

    close(p[0]);
    close(p[1]);
    close(epfd);
}

/* ------------------------------------------------------------------ */
/* Wait tests (epoll_wait01)                                            */
/* ------------------------------------------------------------------ */

static void test_wait(void) {
    printf("\n[T] epoll_wait: timeout and event detection\n");
    int epfd = epoll_create1(0);
    struct epoll_event events[10];

    int ret = epoll_wait(epfd, events, 10, 100);
    if (ret == 0)
        PASS("timeout returned 0");
    else
        FAIL("timeout returned %d (expected 0)", ret);

    int p[2];
    pipe(p);

    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data.fd = p[1];
    epoll_ctl(epfd, EPOLL_CTL_ADD, p[1], &ev);

    ret = epoll_wait(epfd, events, 10, 1000);
    if (ret == 1 && (events[0].events & EPOLLOUT))
        PASS("detected EPOLLOUT on empty pipe write-end");
    else
        FAIL("EPOLLOUT detection failed (ret=%d, events=0x%x)", ret, events[0].events);

    /* Test EPOLLIN */
    ev.events = EPOLLIN;
    ev.data.fd = p[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev);

    write(p[1], "x", 1);
    ret = epoll_wait(epfd, events, 10, 1000);
    if (ret >= 1) {
        int found_in = 0;
        for (int i=0; i<ret; i++) {
            if (events[i].data.fd == p[0] && (events[i].events & EPOLLIN))
                found_in = 1;
        }
        if (found_in)
            PASS("detected EPOLLIN after write");
        else
            FAIL("EPOLLIN not in events (ret=%d)", ret);
    } else {
        FAIL("EPOLLIN detection failed (ret=%d)", ret);
    }

    /* Test both EPOLLIN (p[0]) and EPOLLOUT (p[1]) simultaneously.
     * IMPORTANT: must reset ev.data.fd = p[1] before MOD, otherwise
     * the kernel stores p[0] as the user-data for the p[1] epitem. */
    ev.events = EPOLLOUT;
    ev.data.fd = p[1]; /* ← fix: was stale p[0] from the EPOLLIN add above */
    epoll_ctl(epfd, EPOLL_CTL_MOD, p[1], &ev);

    ret = epoll_wait(epfd, events, 10, 1000);
    if (ret < 2) {
        FAIL("mixed detection: expected 2 events, got %d", ret);
    } else {
        int mask = 0;
        for (int i = 0; i < ret; i++) {
            if (events[i].data.fd == p[0] && (events[i].events & EPOLLIN))  mask |= 1;
            if (events[i].data.fd == p[1] && (events[i].events & EPOLLOUT)) mask |= 2;
        }
        if (mask == 3)
            PASS("detected both EPOLLIN (p[0]) and EPOLLOUT (p[1])");
        else
            FAIL("mixed detection failed (mask=0x%x, ret=%d)", mask, ret);
    }

    close(p[0]);
    close(p[1]);
    close(epfd);
}

/* ------------------------------------------------------------------ */
/* Nested Epoll                                                         */
/* ------------------------------------------------------------------ */

static void test_nested(void) {
    printf("\n[T] Nested epoll: epoll fd in another epoll fd\n");
    int ep1 = epoll_create1(0);
    int ep2 = epoll_create1(0);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = ep2;

    if (epoll_ctl(ep1, EPOLL_CTL_ADD, ep2, &ev) == 0)
        PASS("added epoll fd to another epoll fd");
    else
        FAIL_ERRNO("nested ADD");

    /* Trigger it */
    int p[2];
    pipe(p);
    ev.events = EPOLLIN;
    ev.data.fd = p[0];
    epoll_ctl(ep2, EPOLL_CTL_ADD, p[0], &ev);

    write(p[1], "y", 1);
    
    struct epoll_event events[1];
    int ret = epoll_wait(ep1, events, 1, 1000);
    if (ret == 1 && events[0].data.fd == ep2)
        PASS("nested epoll triggered successfully");
    else
        FAIL("nested trigger failed (ret=%d)", ret);

    close(p[0]);
    close(p[1]);
    close(ep1);
    close(ep2);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("  LTP epoll standalone test suite\n");
    printf("  (ported for AscentOS / musl-static)\n");
    printf("========================================\n");

    test_create();
    test_ctl();
    test_wait();
    test_nested();

    print_summary();
    return g_fail ? 1 : 0;
}
