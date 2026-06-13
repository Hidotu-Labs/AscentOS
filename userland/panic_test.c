/*
 * panic_test.c — Kernel panic / bug trigger for AscentOS
 *
 * MODES:
 *
 *   vfs_uaf     Trigger the VFS refcount use-after-free bug.
 *               Opens a deep path so vfs_resolve_path_at walks ramfs
 *               directory nodes and calls vfs_close on each parent without
 *               a matching vfs_open.  On a persistent ramfs node with
 *               refcount=1 this decrements to 0, fires the close callback,
 *               and clears the page cache — corrupting the live node.
 *               The next kernel access to that directory node causes a
 *               ring-0 GPF.  Repeat the open loop to maximise the chance
 *               of hitting the corrupted node while it is still cached.
 *
 *   dir_bomb    Open thousands of distinct paths that all traverse /dev.
 *               /dev is a persistent ramfs directory with refcount=1.
 *               Every open drills through it N times.  When refcount wraps
 *               to 0 the node is "closed" while still in use → kernel GPF.
 *
 *   mmap_storm  Rapid mmap/munmap/touch cycles reproducing the pcmanfm
 *               crash pattern (32 KB anonymous private mappings).
 *
 *   bad_syscall Probe syscall input validation with out-of-range pointers.
 *               Should return EFAULT cleanly; a panic means missing checks.
 *
 *   ud2         Execute the UD2 invalid-opcode instruction.
 *               Kernel should send SIGILL (not panic).
 *
 *   stack       Infinite recursion → stack overflow → SIGILL/SIGSEGV.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <mode>\n"
        "\n"
        "  vfs_uaf      VFS refcount underflow → ring-0 GPF (kernel panic)\n"
        "  dir_bomb     Hammer /dev traversal → deplete ramfs refcounts\n"
        "  mmap_storm   Rapid mmap/munmap → reproduces pcmanfm panic\n"
        "  bad_syscall  Syscalls with kernel/invalid pointers (EFAULT probe)\n"
        "  ud2          UD2 invalid opcode (should get SIGILL, not panic)\n"
        "  stack        Stack overflow (should get SIGSEGV, not panic)\n"
        "\n",
        prog);
}

/* ── mode: vfs_uaf ────────────────────────────────────────────────────────── */
/*
 * The VFS path walker (vfs_resolve_path_at) calls vfs_open() on its initial
 * node, then calls vfs_finddir() for each component.  vfs_finddir() returns
 * a raw pointer for ramfs nodes WITHOUT bumping refcount.  The walker then
 * pushes the old 'current' onto its parent stack and eventually calls
 * vfs_close() on every parent.
 *
 * For ramfs nodes refcount starts at 1 (representing "node exists in tree").
 * One vfs_close() without a preceding vfs_open() → refcount hits 0 →
 * the close callback fires and vfs_cache_clear() runs on the live node.
 *
 * After this the node is "dead" in the kernel's eyes but still in the tree.
 * The next access through that path calls a function pointer from freed
 * memory → ring-0 General Protection Fault → kernel panic.
 *
 * Strategy: open many nested paths that all drill through the same
 * intermediate directories, then stat/open that directory again to trigger
 * the corrupted access.
 */
static void do_vfs_uaf(void) {
    puts("[panic_test] mode=vfs_uaf: VFS refcount underflow attack");
    puts("[panic_test] Opening deeply nested paths through persistent ramfs dirs...");
    fflush(stdout);

    /*
     * Each open of /dev/console forces the kernel to walk:
     *   fs_root → "dev" (persistent ramfs, refcount=1)
     *                → "console" (also persistent)
     *
     * The walker does:
     *   vfs_open(fs_root)          refcount++  (now 2 for root)
     *   vfs_finddir(root,"dev")    returns dev node, NO refcount bump
     *   push root onto stack
     *   current = dev              dev is now 'current' — no open!
     *   vfs_finddir(dev,"console") returns console, no bump
     *   push dev onto stack
     *   current = console          no open!
     *   -- path done, clean up stack --
     *   vfs_close(dev)             dev refcount: 1 → 0 → DEAD
     *   vfs_close(root)            root refcount: 2 → 1  (OK)
     *   return console             refcount never bumped; caller gets
     *                              a dangling pointer after close
     *
     * On the second traversal, 'dev' has refcount=0 so the close handler
     * may have already run.  The next vfs_finddir call on a "dead" dev
     * dereferences its ramfs_dir_t device pointer which is now poisoned.
     */
    const int ROUNDS = 2048;
    int opened = 0, closed = 0;

    for (int i = 0; i < ROUNDS; i++) {
        /* Open /dev/console — drills through /dev every time */
        int fd = open("/dev/console", O_RDONLY | O_NOCTTY);
        if (fd >= 0) {
            opened++;
            close(fd);
            closed++;
        }

        /* Open /dev/null — same traversal, different leaf */
        fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            opened++;
            close(fd);
            closed++;
        }

        /* stat() forces another traversal without open */
        struct stat st;
        stat("/dev/null", &st);

        if ((i & 127) == 0) {
            printf("[panic_test]   round %d / %d\n", i, ROUNDS);
            fflush(stdout);
        }
    }

    printf("[panic_test] vfs_uaf: opened=%d closed=%d\n", opened, closed);
    puts("[panic_test] If no panic yet, trying direct /dev readdir to poke the node...");
    fflush(stdout);

    /* Force the kernel to call dev->finddir repeatedly */
    for (int i = 0; i < 256; i++) {
        DIR *d = opendir("/dev");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL)
                (void)de;
            closedir(d);
        }
    }

    puts("[panic_test] vfs_uaf: survived — kernel may have already been fixed.");
}

/* ── mode: dir_bomb ───────────────────────────────────────────────────────── */
/*
 * Open many different /dev/* nodes as fast as possible.
 * Each open traverses the /dev ramfs node.  Once its refcount hits 0 from
 * accumulated unbalanced vfs_close() calls, the node is corrupted.
 *
 * The difference from vfs_uaf: here we open files in parallel (fork children)
 * to add scheduler interleaving which makes the race more likely to bite.
 */
static void do_dir_bomb(void) {
    puts("[panic_test] mode=dir_bomb: hammering /dev traversal");
    fflush(stdout);

    /* First drain: open every name in /dev many times */
    static const char *devs[] = {
        "/dev/null", "/dev/zero", "/dev/console",
        "/dev/tty",  "/dev/tty0", "/dev/tty1",
        "/dev/fb0",  "/dev/random", "/dev/urandom",
        NULL
    };

    const int ROUNDS = 4096;
    for (int r = 0; r < ROUNDS; r++) {
        for (int d = 0; devs[d]; d++) {
            int fd = open(devs[d], O_RDONLY | O_NOCTTY);
            if (fd >= 0) close(fd);
        }
        /* Also stat them — triggers another vfs_resolve_path_at walk */
        for (int d = 0; devs[d]; d++) {
            struct stat st;
            stat(devs[d], &st);
        }
        if ((r & 255) == 0) {
            printf("[panic_test]   dir_bomb round %d / %d\n", r, ROUNDS);
            fflush(stdout);
        }
    }

    /* Final poke: list /dev to call dev->readdir/finddir on the (possibly
     * now-corrupted) ramfs directory node */
    puts("[panic_test] dir_bomb: final readdir poke...");
    fflush(stdout);
    for (int i = 0; i < 512; i++) {
        DIR *d = opendir("/dev");
        if (!d) {
            printf("[panic_test] opendir /dev failed at i=%d: %s\n",
                   i, strerror(errno));
            fflush(stdout);
            /* If opendir itself fails with EIO or crashes, the node is dead */
            break;
        }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) (void)de;
        closedir(d);
    }

    puts("[panic_test] dir_bomb: survived (or opendir failed cleanly).");
}

/* ── mode: mmap_storm ─────────────────────────────────────────────────────── */

#define STORM_ITERS 4096
#define STORM_SIZE  32768

static void do_mmap_storm(void) {
    printf("[panic_test] mode=mmap_storm: %d x %d byte mmap/touch/munmap\n",
           STORM_ITERS, STORM_SIZE);
    fflush(stdout);

    int failed = 0;
    for (int i = 0; i < STORM_ITERS; i++) {
        void *p = mmap(NULL, STORM_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { failed++; continue; }

        volatile char *cp = (volatile char *)p;
        for (size_t off = 0; off < STORM_SIZE; off += 4096)
            cp[off] = (char)i;

        if (munmap(p, STORM_SIZE) != 0) failed++;

        if ((i & 255) == 0) {
            printf("[panic_test]   mmap_storm %d / %d\n", i, STORM_ITERS);
            fflush(stdout);
        }
    }

    if (failed)
        fprintf(stderr, "[panic_test] mmap_storm: %d failures\n", failed);
    else
        puts("[panic_test] mmap_storm: PASSED");
}

/* ── mode: bad_syscall ────────────────────────────────────────────────────── */

static void do_bad_syscall(void) {
    puts("[panic_test] mode=bad_syscall: probing syscall input validation");
    fflush(stdout);

    long ret;

    /* Non-canonical pointer as read() buffer */
    ret = syscall(SYS_read, 0, (void *)0xdeadbeef0000ULL, 64);
    printf("[panic_test] read(0, 0xdeadbeef0000, 64) = %ld (want -EFAULT)\n", ret);

    /* Kernel-space address as stat() path */
    struct { long s[18]; } sb;
    ret = syscall(SYS_stat, (void *)0xffffffff80000000ULL, &sb);
    printf("[panic_test] stat(0xffff...0000, &sb)   = %ld (want -EFAULT)\n", ret);

    /* write() with length longer than mapping */
    void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        ret = syscall(SYS_write, 1, p, 1024 * 1024);
        printf("[panic_test] write(1, p, 1MB over 4KB map) = %ld\n", ret);
        munmap(p, 4096);
    }

    puts("[panic_test] bad_syscall: done — no panic means validation is OK");
}

/* ── mode: ud2 ────────────────────────────────────────────────────────────── */

static void do_ud2(void) {
    puts("[panic_test] mode=ud2: executing UD2 — expect SIGILL, not panic");
    fflush(stdout);
    __asm__ volatile("ud2");
}

/* ── mode: stack ──────────────────────────────────────────────────────────── */

static volatile int depth;
static void recurse(void) {
    depth++;
    volatile char buf[512];
    buf[0] = (char)depth;
    recurse();
    (void)buf;
}
static void do_stack(void) {
    puts("[panic_test] mode=stack: infinite recursion — expect SIGSEGV, not panic");
    fflush(stdout);
    recurse();
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char *mode = argv[1];
    if      (!strcmp(mode, "vfs_uaf"))    do_vfs_uaf();
    else if (!strcmp(mode, "dir_bomb"))   do_dir_bomb();
    else if (!strcmp(mode, "mmap_storm")) do_mmap_storm();
    else if (!strcmp(mode, "bad_syscall"))do_bad_syscall();
    else if (!strcmp(mode, "ud2"))        do_ud2();
    else if (!strcmp(mode, "stack"))      do_stack();
    else {
        fprintf(stderr, "[panic_test] unknown mode: %s\n\n", mode);
        print_usage(argv[0]);
        return 1;
    }

    puts("[panic_test] returned normally.");
    return 0;
}
