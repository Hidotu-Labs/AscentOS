/*
 * test_drm_epoll.c — Expanded DRM + epoll integration test
 *
 * Covers every epoll feature weston relies on:
 *   DRM fd (EPOLLIN/EPOLLOUT/ET/ONESHOT/MOD/DEL/EXCLUSIVE)
 *   timerfd  — epoll_wait must return when timer fires (weston repaint loop)
 *   eventfd  — write wakes epoll_wait; semaphore mode; EFD_NONBLOCK
 *   pipe     — write end wakes reader side watched by epoll
 *   unix socket — connect/send wakes epoll on peer fd
 *   signalfd — pending signal readable via epoll
 *   Mixed    — multiple fd types on one epoll, correct fd reported
 *   Stress   — 50-iteration rapid flip/drain cycle
 *
 * Run on AscentOS: /bin/test_drm_epoll
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

/* eventfd — musl exposes it via sys/eventfd.h */
#include <sys/eventfd.h>
#include <stddef.h>

/* ── DRM ioctl numbers ───────────────────────────────────────────────────── */
#define DRM_IOCTL_MODE_GETRESOURCES  0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC       0xC06864A1
#define DRM_IOCTL_MODE_SETCRTC       0xC06864A2
#define DRM_IOCTL_MODE_ADDFB         0xC01C64AE
#define DRM_IOCTL_MODE_RMFB          0xC00464AF
#define DRM_IOCTL_MODE_PAGE_FLIP     0x401864B0
#define DRM_IOCTL_MODE_CREATE_DUMB   0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB      0xC01064B3

/* ── DRM structs ─────────────────────────────────────────────────────────── */
struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, mode_valid;
    struct drm_mode_modeinfo mode;
};
struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};
struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_crtc_page_flip {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
};
#define DRM_MODE_PAGE_FLIP_EVENT 0x01
struct drm_event        { uint32_t type, length; };
#define DRM_EVENT_FLIP_COMPLETE 0x02
struct drm_event_vblank {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec, tv_usec, sequence, crtc_id;
};

/* ── Test helpers ────────────────────────────────────────────────────────── */
static int pass_count = 0;
static int fail_count = 0;
#define PASS(msg) do { printf("  [PASS] %s\n", msg); pass_count++; } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s (errno=%d: %s)\n", msg, errno, strerror(errno)); fail_count++; } while(0)
#define CHECK(cond, msg) do { if (cond) PASS(msg); else FAIL(msg); } while(0)
#define SKIP(msg) do { printf("  [SKIP] %s\n", msg); } while(0)

/* ── DRM helpers ─────────────────────────────────────────────────────────── */
static int drm_open_and_setup(uint32_t *out_crtc, uint32_t *out_fb,
                               uint32_t **out_ptr, uint64_t *out_size) {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) return -1;
    struct drm_mode_card_res res = {0};
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    if (res.count_crtcs == 0) { close(fd); return -1; }
    uint32_t *crtc_ids = malloc(res.count_crtcs * sizeof(uint32_t));
    res.crtc_id_ptr = (uintptr_t)crtc_ids;
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    *out_crtc = crtc_ids[0];
    free(crtc_ids);
    struct drm_mode_create_dumb cre = {0};
    cre.width = 1280; cre.height = 800; cre.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cre) < 0) { close(fd); return -1; }
    struct drm_mode_map_dumb map = {0};
    map.handle = cre.handle;
    ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    uint32_t *ptr = mmap(NULL, cre.size, PROT_READ|PROT_WRITE,
                         MAP_SHARED, fd, (off_t)map.offset);
    if (ptr == MAP_FAILED) { close(fd); return -1; }
    for (uint64_t i = 0; i < cre.size/4; i++) ptr[i] = 0x003366FF;
    struct drm_mode_fb_cmd fb_cmd = {0};
    fb_cmd.width = 1280; fb_cmd.height = 800;
    fb_cmd.handle = cre.handle; fb_cmd.bpp = 32; fb_cmd.pitch = cre.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd) < 0) { close(fd); return -1; }
    *out_fb = fb_cmd.fb_id;
    struct drm_mode_crtc crtc_set = {0};
    crtc_set.crtc_id = *out_crtc; crtc_set.fb_id = *out_fb;
    crtc_set.mode.hdisplay = 1280; crtc_set.mode.vdisplay = 800;
    ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc_set);
    if (out_ptr)  *out_ptr  = ptr;
    if (out_size) *out_size = cre.size;
    return fd;
}
static int do_flip(int fd, uint32_t crtc_id, uint32_t fb_id, uint64_t udata) {
    struct drm_mode_crtc_page_flip flip = {0};
    flip.crtc_id = crtc_id; flip.fb_id = fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT; flip.user_data = udata;
    return ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip);
}
static int drain_event(int fd) {
    struct drm_event_vblank ev;
    return (int)read(fd, &ev, sizeof(ev));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION A — DRM fd tests (same as before, condensed)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_drm_lt_basic(int epfd, int drm_fd, uint32_t crtc, uint32_t fb) {
    printf("\n[A1] DRM LT-EPOLLIN basic\n");
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = drm_fd };
    CHECK(epoll_ctl(epfd, EPOLL_CTL_ADD, drm_fd, &ev) == 0, "CTL_ADD drm_fd");
    struct epoll_event evs[8];
    CHECK(epoll_wait(epfd, evs, 8, 0) == 0, "wait==0 before flip");
    do_flip(drm_fd, crtc, fb, 0xA1);
    int n = epoll_wait(epfd, evs, 8, 0);
    CHECK(n == 1, "wait==1 after flip");
    if (n >= 1) {
        CHECK((evs[0].events & EPOLLIN) != 0, "EPOLLIN set");
        CHECK(evs[0].data.fd == drm_fd, "data.fd correct");
    }
    /* LT: stays until drained */
    CHECK(epoll_wait(epfd, evs, 8, 0) == 1, "LT: still 1 before drain");
    drain_event(drm_fd);
    CHECK(epoll_wait(epfd, evs, 8, 0) == 0, "LT: 0 after drain");
}

static void test_drm_epollout(int epfd, int drm_fd) {
    printf("\n[A2] DRM EPOLLOUT always-ready\n");
    struct epoll_event ev = { .events = EPOLLIN|EPOLLOUT, .data.fd = drm_fd };
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
    struct epoll_event evs[8];
    int n = epoll_wait(epfd, evs, 8, 0);
    CHECK(n == 1, "EPOLLOUT: wait==1");
    if (n >= 1) CHECK((evs[0].events & EPOLLOUT) != 0, "EPOLLOUT set");
    ev.events = EPOLLIN; ev.data.fd = drm_fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
}

static void test_drm_epollet(int epfd, int drm_fd, uint32_t crtc, uint32_t fb) {
    printf("\n[A3] DRM EPOLLET edge-triggered\n");
    struct epoll_event ev = { .events = EPOLLIN|EPOLLET, .data.fd = drm_fd };
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
    do_flip(drm_fd, crtc, fb, 0xA3);
    struct epoll_event evs[8];
    CHECK(epoll_wait(epfd, evs, 8, 0) == 1, "ET: fires on edge");
    CHECK(epoll_wait(epfd, evs, 8, 0) == 0, "ET: silent without new edge");
    drain_event(drm_fd);
    do_flip(drm_fd, crtc, fb, 0xA3b);
    CHECK(epoll_wait(epfd, evs, 8, 0) == 1, "ET: new flip = new edge");
    drain_event(drm_fd);
    ev.events = EPOLLIN; ev.data.fd = drm_fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
}

static void test_drm_oneshot(int epfd, int drm_fd, uint32_t crtc, uint32_t fb) {
    printf("\n[A4] DRM EPOLLONESHOT\n");
    struct epoll_event ev = { .events = EPOLLIN|EPOLLONESHOT, .data.fd = drm_fd };
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
    do_flip(drm_fd, crtc, fb, 0xA4);
    struct epoll_event evs[8];
    CHECK(epoll_wait(epfd, evs, 8, 0) == 1, "ONESHOT: fires once");
    drain_event(drm_fd);
    do_flip(drm_fd, crtc, fb, 0xA4b);
    CHECK(epoll_wait(epfd, evs, 8, 0) == 0, "ONESHOT: silent after fire");
    drain_event(drm_fd);
    ev.events = EPOLLIN|EPOLLONESHOT; ev.data.fd = drm_fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
    do_flip(drm_fd, crtc, fb, 0xA4c);
    CHECK(epoll_wait(epfd, evs, 8, 0) == 1, "ONESHOT: fires after re-arm");
    drain_event(drm_fd);
    ev.events = EPOLLIN; ev.data.fd = drm_fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
}

static void test_drm_timeout(int epfd, int drm_fd, uint32_t crtc, uint32_t fb) {
    printf("\n[A5] DRM epoll_wait timeouts\n");
    do_flip(drm_fd, crtc, fb, 0xA5);
    struct epoll_event evs[8];
    CHECK(epoll_wait(epfd, evs, 8, 200) == 1, "timeout=200ms, event pre-queued");
    drain_event(drm_fd);
    CHECK(epoll_wait(epfd, evs, 8, 1) == 0,   "timeout=1ms, no event → 0");
    do_flip(drm_fd, crtc, fb, 0xA5b);
    CHECK(epoll_wait(epfd, evs, 8, -1) == 1,  "timeout=-1, event pre-queued");
    drain_event(drm_fd);
}

static void test_drm_ctl_errors(int epfd, int drm_fd) {
    printf("\n[A6] CTL error codes\n");
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = drm_fd };
    int r = epoll_ctl(epfd, EPOLL_CTL_ADD, drm_fd, &ev);
    CHECK(r == -1 && errno == EEXIST, "ADD duplicate → EEXIST");
    int tmp = open("/dev/dri/card0", O_RDWR);
    if (tmp >= 0) {
        r = epoll_ctl(epfd, EPOLL_CTL_MOD, tmp, &ev);
        CHECK(r == -1 && errno == ENOENT, "MOD unregistered → ENOENT");
        close(tmp);
    }
}

static void test_drm_data_union(int epfd, int drm_fd, uint32_t crtc, uint32_t fb) {
    printf("\n[A7] data union round-trip\n");
    struct epoll_event ev; struct epoll_event evs[8];
    ev.events = EPOLLIN; ev.data.u64 = 0xCAFEBABEDEAD1234ULL;
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
    do_flip(drm_fd, crtc, fb, 0xA7a);
    int n = epoll_wait(epfd, evs, 8, 0);
    if (n >= 1) CHECK(evs[0].data.u64 == 0xCAFEBABEDEAD1234ULL, "data.u64");
    else FAIL("data.u64: no event");
    drain_event(drm_fd);
    int sentinel = 42;
    ev.data.ptr = &sentinel;
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
    do_flip(drm_fd, crtc, fb, 0xA7b);
    n = epoll_wait(epfd, evs, 8, 0);
    if (n >= 1) CHECK(evs[0].data.ptr == (void*)&sentinel, "data.ptr");
    else FAIL("data.ptr: no event");
    drain_event(drm_fd);
    ev.events = EPOLLIN; ev.data.fd = drm_fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, drm_fd, &ev);
}

static void test_drm_stress(int epfd, int drm_fd, uint32_t crtc, uint32_t fb) {
    printf("\n[A8] Rapid flip/drain stress (50 iterations)\n");
    struct epoll_event evs[4]; int ok = 1;
    for (int i = 0; i < 50; i++) {
        do_flip(drm_fd, crtc, fb, (uint64_t)i);
        if (epoll_wait(epfd, evs, 4, 0) != 1) { ok = 0; break; }
        drain_event(drm_fd);
        if (epoll_wait(epfd, evs, 4, 0) != 0) { ok = 0; break; }
    }
    CHECK(ok, "50x flip→wait→drain all correct");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION B — timerfd (weston repaint deadline timer)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_timerfd(int epfd) {
    printf("\n[B1] timerfd one-shot fires via epoll_wait\n");

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
    if (tfd < 0) { SKIP("timerfd_create failed"); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = tfd };
    CHECK(epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev) == 0, "CTL_ADD timerfd");

    /* Arm for 50 ms */
    struct itimerspec its = {0};
    its.it_value.tv_sec  = 0;
    its.it_value.tv_nsec = 50 * 1000000; /* 50 ms */
    CHECK(timerfd_settime(tfd, 0, &its, NULL) == 0, "timerfd_settime 50ms");

    /* epoll_wait must return within ~500ms */
    struct epoll_event evs[4];
    int n = epoll_wait(epfd, evs, 4, 500);
    CHECK(n == 1, "timerfd: epoll_wait returns 1 when timer fires");
    if (n >= 1) {
        CHECK(evs[0].data.fd == tfd, "timerfd: correct fd reported");
        CHECK((evs[0].events & EPOLLIN) != 0, "timerfd: EPOLLIN set");
        uint64_t exp = 0;
        int r = (int)read(tfd, &exp, sizeof(exp));
        CHECK(r == 8 && exp >= 1, "timerfd: read returns expiration count >= 1");
    }

    printf("\n[B2] timerfd periodic fires multiple times\n");
    /* Arm periodic 30ms */
    its.it_value.tv_sec     = 0;
    its.it_value.tv_nsec    = 30 * 1000000;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = 30 * 1000000;
    timerfd_settime(tfd, 0, &its, NULL);

    int fires = 0;
    for (int i = 0; i < 3; i++) {
        n = epoll_wait(epfd, evs, 4, 300);
        if (n == 1 && evs[0].data.fd == tfd) {
            uint64_t exp = 0;
            read(tfd, &exp, sizeof(exp));
            fires++;
        }
    }
    CHECK(fires == 3, "timerfd periodic: fired 3 times");

    /* Disarm */
    memset(&its, 0, sizeof(its));
    timerfd_settime(tfd, 0, &its, NULL);

    printf("\n[B3] timerfd disarmed: epoll_wait(50ms) returns 0\n");
    n = epoll_wait(epfd, evs, 4, 50);
    CHECK(n == 0, "timerfd disarmed: no event");

    printf("\n[B4] timerfd already-expired: epoll_wait(0) returns 1\n");
    its.it_value.tv_sec = 0; its.it_value.tv_nsec = 1; /* 1 ns — fires immediately */
    timerfd_settime(tfd, 0, &its, NULL);
    /* Spin briefly to let it expire */
    volatile int spin = 0; for (int i = 0; i < 100000; i++) spin++;
    n = epoll_wait(epfd, evs, 4, 0);
    CHECK(n == 1, "timerfd already-expired: epoll_wait(0) returns 1");
    if (n == 1) { uint64_t exp = 0; read(tfd, &exp, sizeof(exp)); }

    epoll_ctl(epfd, EPOLL_CTL_DEL, tfd, NULL);
    close(tfd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION C — eventfd (weston uses this for wl_event_loop wakeup)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_eventfd(int epfd) {
    printf("\n[C1] eventfd basic write→epoll_wait\n");

    int efd = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
    if (efd < 0) { SKIP("eventfd failed"); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = efd };
    CHECK(epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev) == 0, "CTL_ADD eventfd");

    struct epoll_event evs[4];
    CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "eventfd: no event before write");

    uint64_t val = 1;
    CHECK(write(efd, &val, 8) == 8, "eventfd: write(1) succeeds");

    int n = epoll_wait(epfd, evs, 4, 0);
    CHECK(n == 1, "eventfd: epoll_wait returns 1 after write");
    if (n >= 1) CHECK((evs[0].events & EPOLLIN) != 0, "eventfd: EPOLLIN set");

    uint64_t rval = 0;
    CHECK(read(efd, &rval, 8) == 8, "eventfd: read returns 8 bytes");
    CHECK(rval == 1, "eventfd: read value == 1");
    CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "eventfd: 0 after drain");

    printf("\n[C2] eventfd accumulates writes\n");
    val = 3; write(efd, &val, 8);
    val = 5; write(efd, &val, 8);
    n = epoll_wait(epfd, evs, 4, 0);
    CHECK(n == 1, "eventfd: 1 event for accumulated writes");
    rval = 0; read(efd, &rval, 8);
    CHECK(rval == 8, "eventfd: accumulated value == 8");

    printf("\n[C3] eventfd EFD_SEMAPHORE mode\n");
    int sefd = eventfd(0, EFD_NONBLOCK|EFD_SEMAPHORE);
    if (sefd < 0) { SKIP("eventfd semaphore failed"); goto done_eventfd; }
    struct epoll_event sev = { .events = EPOLLIN, .data.fd = sefd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, sefd, &sev);
    val = 3; write(sefd, &val, 8);
    /* Each read in semaphore mode returns 1 and decrements by 1 */
    int sem_ok = 1;
    for (int i = 0; i < 3; i++) {
        n = epoll_wait(epfd, evs, 4, 0);
        if (n != 1) { sem_ok = 0; break; }
        rval = 0; read(sefd, &rval, 8);
        if (rval != 1) { sem_ok = 0; break; }
    }
    CHECK(sem_ok, "EFD_SEMAPHORE: 3 reads each return 1");
    CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "EFD_SEMAPHORE: empty after 3 reads");
    epoll_ctl(epfd, EPOLL_CTL_DEL, sefd, NULL);
    close(sefd);

done_eventfd:
    epoll_ctl(epfd, EPOLL_CTL_DEL, efd, NULL);
    close(efd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION D — pipe (weston uses pipe for self-pipe wakeup trick)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_pipe(int epfd) {
    printf("\n[D1] pipe: write wakes epoll on read end\n");

    int pfd[2];
    if (pipe(pfd) < 0) { SKIP("pipe failed"); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = pfd[0] };
    CHECK(epoll_ctl(epfd, EPOLL_CTL_ADD, pfd[0], &ev) == 0, "CTL_ADD pipe[0]");

    struct epoll_event evs[4];
    CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "pipe: no event before write");

    char byte = 'W';
    CHECK(write(pfd[1], &byte, 1) == 1, "pipe: write 1 byte");

    int n = epoll_wait(epfd, evs, 4, 0);
    CHECK(n == 1, "pipe: epoll_wait returns 1 after write");
    if (n >= 1) {
        CHECK(evs[0].data.fd == pfd[0], "pipe: correct fd");
        CHECK((evs[0].events & EPOLLIN) != 0, "pipe: EPOLLIN set");
    }

    char rbuf[4] = {0};
    read(pfd[0], rbuf, 1);
    CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "pipe: 0 after drain");

    printf("\n[D2] pipe: multiple bytes, LT stays until fully drained\n");
    write(pfd[1], "ABC", 3);
    CHECK(epoll_wait(epfd, evs, 4, 0) == 1, "pipe: 1 event for 3 bytes");
    read(pfd[0], rbuf, 2); /* partial drain */
    CHECK(epoll_wait(epfd, evs, 4, 0) == 1, "pipe LT: still 1 after partial drain");
    read(pfd[0], rbuf, 1); /* full drain */
    CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "pipe LT: 0 after full drain");

    epoll_ctl(epfd, EPOLL_CTL_DEL, pfd[0], NULL);
    close(pfd[0]);
    close(pfd[1]);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION E — unix socket (weston's Wayland protocol socket)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_unix_socket(int epfd) {
    printf("\n[E1] unix socket: connect wakes listener epoll\n");

    int srv = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK, 0);
    if (srv < 0) { SKIP("socket(AF_UNIX) failed"); return; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    /* Abstract namespace: first byte is NUL */
    addr.sun_path[0] = '\0';
    strcpy(addr.sun_path + 1, "test_epoll_weston");
    int addrlen = offsetof(struct sockaddr_un, sun_path) + 1 + 17;

    if (bind(srv, (struct sockaddr*)&addr, addrlen) < 0) {
        SKIP("bind abstract socket failed"); close(srv); return;
    }
    if (listen(srv, 4) < 0) {
        SKIP("listen failed"); close(srv); return;
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv };
    CHECK(epoll_ctl(epfd, EPOLL_CTL_ADD, srv, &ev) == 0, "CTL_ADD unix srv");

    struct epoll_event evs[4];
    CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "unix: no event before connect");

    /* Connect from a client socket */
    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cli < 0) { SKIP("client socket failed"); goto done_unix; }
    int cr = connect(cli, (struct sockaddr*)&addr, addrlen);
    CHECK(cr == 0 || (cr < 0 && errno == EINPROGRESS),
          "unix: connect returns 0 or EINPROGRESS");

    int n = epoll_wait(epfd, evs, 4, 100);
    CHECK(n == 1, "unix: epoll_wait returns 1 after connect");
    if (n >= 1) {
        CHECK(evs[0].data.fd == srv, "unix: correct fd (server)");
        CHECK((evs[0].events & EPOLLIN) != 0, "unix: EPOLLIN on server");
    }

    /* Accept the connection */
    int acc = accept(srv, NULL, NULL);
    CHECK(acc >= 0, "unix: accept succeeds");

    printf("\n[E2] unix socket: send data wakes reader epoll\n");
    if (acc >= 0) {
        struct epoll_event ev2 = { .events = EPOLLIN, .data.fd = acc };
        epoll_ctl(epfd, EPOLL_CTL_ADD, acc, &ev2);

        CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "unix: no data before send");

        const char *msg = "hello weston";
        CHECK(send(cli, msg, strlen(msg), 0) == (int)strlen(msg),
              "unix: send succeeds");

        n = epoll_wait(epfd, evs, 4, 100);
        CHECK(n == 1, "unix: epoll_wait returns 1 after send");
        if (n >= 1) {
            CHECK(evs[0].data.fd == acc, "unix: correct fd (accepted)");
            CHECK((evs[0].events & EPOLLIN) != 0, "unix: EPOLLIN on accepted");
        }

        char rbuf[64] = {0};
        recv(acc, rbuf, sizeof(rbuf), 0);
        CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "unix: 0 after recv drain");

        printf("\n[E3] unix socket: EPOLLRDHUP on peer close\n");
        close(cli); cli = -1;
        n = epoll_wait(epfd, evs, 4, 100);
        CHECK(n == 1, "unix: epoll fires on peer close");
        if (n >= 1)
            CHECK((evs[0].events & (EPOLLHUP|EPOLLRDHUP|EPOLLIN)) != 0,
                  "unix: EPOLLHUP/RDHUP/IN on peer close");

        epoll_ctl(epfd, EPOLL_CTL_DEL, acc, NULL);
        close(acc);
    }

done_unix:
    if (cli >= 0) close(cli);
    epoll_ctl(epfd, EPOLL_CTL_DEL, srv, NULL);
    close(srv);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION F — signalfd (weston uses this for SIGCHLD/SIGTERM)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_signalfd(int epfd) {
    printf("\n[F1] signalfd: pending signal readable via epoll\n");

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sfd = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC);
    if (sfd < 0) { SKIP("signalfd failed"); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = sfd };
    CHECK(epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev) == 0, "CTL_ADD signalfd");

    struct epoll_event evs[4];
    CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "signalfd: no event before signal");

    raise(SIGUSR1);

    int n = epoll_wait(epfd, evs, 4, 100);
    CHECK(n == 1, "signalfd: epoll_wait returns 1 after raise(SIGUSR1)");
    if (n >= 1) {
        CHECK(evs[0].data.fd == sfd, "signalfd: correct fd");
        CHECK((evs[0].events & EPOLLIN) != 0, "signalfd: EPOLLIN set");
        struct signalfd_siginfo si;
        int r = (int)read(sfd, &si, sizeof(si));
        CHECK(r == (int)sizeof(si), "signalfd: read returns sizeof(siginfo)");
        CHECK(si.ssi_signo == SIGUSR1, "signalfd: ssi_signo == SIGUSR1");
    }

    CHECK(epoll_wait(epfd, evs, 4, 0) == 0, "signalfd: 0 after drain");

    epoll_ctl(epfd, EPOLL_CTL_DEL, sfd, NULL);
    close(sfd);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION G — Mixed fd types on one epoll (weston's actual event loop shape)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_mixed(int drm_fd, uint32_t crtc, uint32_t fb) {
    printf("\n[G1] Mixed: timerfd + eventfd + pipe + DRM on one epoll\n");

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { SKIP("epoll_create1 failed"); return; }

    /* timerfd */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    /* eventfd */
    int efd = eventfd(0, EFD_NONBLOCK);
    /* pipe */
    int pfd[2]; pipe(pfd);

    if (tfd < 0 || efd < 0) {
        SKIP("timerfd or eventfd unavailable");
        if (tfd >= 0) close(tfd);
        if (efd >= 0) close(efd);
        close(pfd[0]); close(pfd[1]);
        close(epfd);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = tfd;   epoll_ctl(epfd, EPOLL_CTL_ADD, tfd,    &ev);
    ev.events = EPOLLIN; ev.data.fd = efd;   epoll_ctl(epfd, EPOLL_CTL_ADD, efd,    &ev);
    ev.events = EPOLLIN; ev.data.fd = pfd[0];epoll_ctl(epfd, EPOLL_CTL_ADD, pfd[0], &ev);
    ev.events = EPOLLIN; ev.data.fd = drm_fd;epoll_ctl(epfd, EPOLL_CTL_ADD, drm_fd, &ev);

    struct epoll_event evs[8];
    CHECK(epoll_wait(epfd, evs, 8, 0) == 0, "mixed: 0 events initially");

    /* Fire all four simultaneously */
    struct itimerspec its = {0};
    its.it_value.tv_nsec = 1; /* 1 ns — fires immediately */
    timerfd_settime(tfd, 0, &its, NULL);
    volatile int spin = 0; for (int i = 0; i < 200000; i++) spin++;

    uint64_t val = 1; write(efd, &val, 8);
    char byte = 'X'; write(pfd[1], &byte, 1);
    do_flip(drm_fd, crtc, fb, 0xA001);

    int n = epoll_wait(epfd, evs, 8, 200);
    CHECK(n == 4, "mixed: all 4 fds fire simultaneously");

    /* Verify each fd is represented */
    int saw_tfd = 0, saw_efd = 0, saw_pipe = 0, saw_drm = 0;
    for (int i = 0; i < n; i++) {
        if (evs[i].data.fd == tfd)    saw_tfd  = 1;
        if (evs[i].data.fd == efd)    saw_efd  = 1;
        if (evs[i].data.fd == pfd[0]) saw_pipe = 1;
        if (evs[i].data.fd == drm_fd) saw_drm  = 1;
    }
    CHECK(saw_tfd,  "mixed: timerfd reported");
    CHECK(saw_efd,  "mixed: eventfd reported");
    CHECK(saw_pipe, "mixed: pipe reported");
    CHECK(saw_drm,  "mixed: DRM reported");

    /* Drain all */
    { uint64_t exp = 0; read(tfd, &exp, sizeof(exp)); }
    { uint64_t rval = 0; read(efd, &rval, sizeof(rval)); }
    { char rb; read(pfd[0], &rb, 1); }
    drain_event(drm_fd);

    CHECK(epoll_wait(epfd, evs, 8, 0) == 0, "mixed: 0 after all drained");

    printf("\n[G2] Mixed: only one fd fires, correct fd returned\n");
    val = 7; write(efd, &val, 8);
    n = epoll_wait(epfd, evs, 8, 50);
    CHECK(n == 1, "mixed: only 1 event when only eventfd fires");
    if (n == 1) CHECK(evs[0].data.fd == efd, "mixed: eventfd fd correct");
    { uint64_t rval = 0; read(efd, &rval, sizeof(rval)); }

    close(tfd); close(efd); close(pfd[0]); close(pfd[1]);
    close(epfd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION H — EPOLLEXCLUSIVE (weston multi-thread guard)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_exclusive(int drm_fd, uint32_t crtc, uint32_t fb) {
    printf("\n[H1] EPOLLEXCLUSIVE: only one of two epoll instances woken\n");
    int ep1 = epoll_create1(0), ep2 = epoll_create1(0);
    if (ep1 < 0 || ep2 < 0) {
        SKIP("epoll_create1 failed");
        if (ep1 >= 0) close(ep1);
        if (ep2 >= 0) close(ep2);
        return;
    }
    struct epoll_event ev1 = { .events = EPOLLIN|EPOLLEXCLUSIVE, .data.u32 = 1 };
    struct epoll_event ev2 = { .events = EPOLLIN|EPOLLEXCLUSIVE, .data.u32 = 2 };
    epoll_ctl(ep1, EPOLL_CTL_ADD, drm_fd, &ev1);
    epoll_ctl(ep2, EPOLL_CTL_ADD, drm_fd, &ev2);
    do_flip(drm_fd, crtc, fb, 0xEC10);
    struct epoll_event evs[4];
    int n1 = epoll_wait(ep1, evs, 4, 0);
    int n2 = epoll_wait(ep2, evs, 4, 0);
    CHECK(n1 + n2 == 1, "EXCLUSIVE: exactly one instance woken");
    drain_event(drm_fd);
    epoll_ctl(ep1, EPOLL_CTL_DEL, drm_fd, NULL);
    epoll_ctl(ep2, EPOLL_CTL_DEL, drm_fd, NULL);
    close(ep1); close(ep2);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION I — timerfd timeout path: epoll_wait(timeout) expires correctly
 *             This is the exact scenario that was broken before the fix.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_timeout_expires(void) {
    printf("\n[I1] epoll_wait timeout expires with no events (regression)\n");

    int epfd = epoll_create1(0);
    if (epfd < 0) { SKIP("epoll_create1 failed"); return; }

    /* Watch a timerfd that we deliberately do NOT arm — so nothing fires */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0) { SKIP("timerfd_create failed"); close(epfd); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = tfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);

    struct epoll_event evs[4];

    /* timeout=1ms must return 0, not hang */
    int n = epoll_wait(epfd, evs, 4, 1);
    CHECK(n == 0, "timeout=1ms returns 0 (not stuck)");

    /* timeout=5ms must return 0, not hang */
    n = epoll_wait(epfd, evs, 4, 5);
    CHECK(n == 0, "timeout=5ms returns 0 (not stuck)");

    /* timeout=50ms must return 0, not hang */
    n = epoll_wait(epfd, evs, 4, 50);
    CHECK(n == 0, "timeout=50ms returns 0 (not stuck)");

    printf("\n[I2] epoll_wait timeout=0 on empty epoll returns 0 immediately\n");
    n = epoll_wait(epfd, evs, 4, 0);
    CHECK(n == 0, "timeout=0 on empty epoll returns 0");

    printf("\n[I3] epoll_wait timeout fires, then event arrives after re-call\n");
    n = epoll_wait(epfd, evs, 4, 10);
    CHECK(n == 0, "first call times out");
    /* Now arm the timer and call again */
    struct itimerspec its = {0};
    its.it_value.tv_nsec = 20 * 1000000; /* 20ms */
    timerfd_settime(tfd, 0, &its, NULL);
    n = epoll_wait(epfd, evs, 4, 200);
    CHECK(n == 1, "second call returns 1 when timer fires");
    if (n == 1) { uint64_t exp = 0; read(tfd, &exp, sizeof(exp)); }

    close(tfd);
    close(epfd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("=== DRM + epoll expanded integration test (weston coverage) ===\n");

    uint32_t crtc_id = 0, fb_id = 0;
    uint32_t *fb_ptr = NULL; uint64_t fb_size = 0;
    int drm_fd = drm_open_and_setup(&crtc_id, &fb_id, &fb_ptr, &fb_size);
    if (drm_fd < 0) { printf("[FATAL] DRM setup failed\n"); return 1; }
    printf("[SETUP] drm_fd=%d crtc=%u fb=%u\n", drm_fd, crtc_id, fb_id);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { printf("[FATAL] epoll_create1 failed\n"); close(drm_fd); return 1; }
    printf("[SETUP] epfd=%d\n\n", epfd);

    /* ── Section A: DRM fd ── */
    test_drm_lt_basic(epfd, drm_fd, crtc_id, fb_id);
    test_drm_epollout(epfd, drm_fd);
    test_drm_epollet(epfd, drm_fd, crtc_id, fb_id);
    test_drm_oneshot(epfd, drm_fd, crtc_id, fb_id);
    test_drm_timeout(epfd, drm_fd, crtc_id, fb_id);
    test_drm_ctl_errors(epfd, drm_fd);
    test_drm_data_union(epfd, drm_fd, crtc_id, fb_id);
    test_drm_stress(epfd, drm_fd, crtc_id, fb_id);

    /* ── Section B: timerfd ── */
    test_timerfd(epfd);

    /* ── Section C: eventfd ── */
    test_eventfd(epfd);

    /* ── Section D: pipe ── */
    test_pipe(epfd);

    /* ── Section E: unix socket ── */
    test_unix_socket(epfd);

    /* ── Section F: signalfd ── */
    test_signalfd(epfd);

    /* ── Section G: mixed fd types ── */
    test_mixed(drm_fd, crtc_id, fb_id);

    /* ── Section H: EPOLLEXCLUSIVE ── */
    test_exclusive(drm_fd, crtc_id, fb_id);

    /* ── Section I: timeout regression ── */
    test_timeout_expires();

    close(epfd);
    if (fb_ptr && fb_ptr != MAP_FAILED) munmap(fb_ptr, fb_size);
    close(drm_fd);

    printf("\n=== Results: %d passed, %d failed ===\n", pass_count, fail_count);
    return (fail_count == 0) ? 0 : 1;
}
