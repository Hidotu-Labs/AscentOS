/*
 * test_syscall_speed.c - AscentOS / Linux Comprehensive Syscall Benchmark Suite
 *
 * Measures the execution speed of syscalls across 8 major categories and
 * multiple operational situations (Valid/Success path, Invalid/Error validation,
 * Payload scale variations, and Flag variations).
 *
 * Each test scenario runs for 1,000 iterations by default.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/inotify.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/uio.h>
#include <sys/prctl.h>
#include <sched.h>
#include <linux/futex.h>

#define DEFAULT_ITERATIONS 1000

#ifndef PR_GET_DUMPABLE
#define PR_GET_DUMPABLE 1
#endif

#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#endif

#ifndef SYS_newfstatat
#ifdef __NR_newfstatat
#define SYS_newfstatat __NR_newfstatat
#elif defined(SYS_fstatat)
#define SYS_newfstatat SYS_fstatat
#endif
#endif

#ifndef SYS_access
#ifdef __NR_access
#define SYS_access __NR_access
#endif
#endif

/* Fallback definitions for syscalls if missing from toolchain headers */
#ifndef SYS_gettid
#define SYS_gettid __NR_gettid
#endif

#ifndef SYS_statx
#ifdef __NR_statx
#define SYS_statx __NR_statx
#endif
#endif

#ifndef SYS_faccessat2
#ifdef __NR_faccessat2
#define SYS_faccessat2 __NR_faccessat2
#endif
#endif

#ifndef SYS_close_range
#ifdef __NR_close_range
#define SYS_close_range __NR_close_range
#endif
#endif

#ifndef SYS_memfd_create
#ifdef __NR_memfd_create
#define SYS_memfd_create __NR_memfd_create
#endif
#endif

#ifndef SYS_getrandom
#ifdef __NR_getrandom
#define SYS_getrandom __NR_getrandom
#endif
#endif

typedef struct {
    const char *category;
    const char *syscall_name;
    const char *situation;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    double avg_ns;
    double ops_per_sec;
    int success_count;
    int error_count;
} bench_result_t;

static bench_result_t results[512];
static size_t num_results = 0;

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void add_result(const char *category, const char *syscall_name,
                       const char *situation, uint64_t total_ns,
                       uint64_t min_ns, uint64_t max_ns,
                       unsigned iterations, int successes, int errors) {
    if (num_results >= sizeof(results)/sizeof(results[0])) return;
    bench_result_t *res = &results[num_results++];
    res->category = category;
    res->syscall_name = syscall_name;
    res->situation = situation;
    res->total_ns = total_ns;
    res->min_ns = min_ns;
    res->max_ns = max_ns;
    res->avg_ns = (double)total_ns / (double)iterations;
    res->ops_per_sec = (total_ns > 0) ? ((double)iterations * 1e9 / (double)total_ns) : 0.0;
    res->success_count = successes;
    res->error_count = errors;
}

/* Helper macro for benchmark loops */
#define BENCHMARK_START() \
    uint64_t _start_total = get_time_ns(); \
    uint64_t _min_ns = (uint64_t)-1; \
    uint64_t _max_ns = 0; \
    int _succ = 0, _err = 0;

#define BENCHMARK_ITER_START() \
    uint64_t _t0 = get_time_ns();

#define BENCHMARK_ITER_END(ret_cond) \
    uint64_t _t1 = get_time_ns(); \
    uint64_t _diff = _t1 - _t0; \
    if (_diff < _min_ns) _min_ns = _diff; \
    if (_diff > _max_ns) _max_ns = _diff; \
    if (ret_cond) _succ++; else _err++;

#define BENCHMARK_FINISH(category, sysname, situation, iterations) \
    uint64_t _end_total = get_time_ns(); \
    add_result(category, sysname, situation, _end_total - _start_total, _min_ns, _max_ns, iterations, _succ, _err);

/* =========================================================================
 * Category 1: Process, Thread & ID Syscalls
 * ========================================================================= */
static void bench_process_thread(unsigned iter) {
    const char *cat = "Process/Thread";

    // getpid (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            pid_t p = syscall(SYS_getpid);
            BENCHMARK_ITER_END(p > 0);
        }
        BENCHMARK_FINISH(cat, "getpid", "Valid / Success Path", iter);
    }

    // getppid (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            pid_t p = syscall(SYS_getppid);
            BENCHMARK_ITER_END(p >= 0);
        }
        BENCHMARK_FINISH(cat, "getppid", "Valid / Success Path", iter);
    }

    // gettid (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            pid_t t = syscall(SYS_gettid);
            BENCHMARK_ITER_END(t > 0);
        }
        BENCHMARK_FINISH(cat, "gettid", "Valid / Success Path", iter);
    }

    // getuid / geteuid / getgid / getegid
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            uid_t u = syscall(SYS_getuid);
            BENCHMARK_ITER_END(u != (uid_t)-1);
        }
        BENCHMARK_FINISH(cat, "getuid", "Valid / Success Path", iter);
    }

    // getpgid (Valid: self)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            pid_t pg = syscall(SYS_getpgid, 0);
            BENCHMARK_ITER_END(pg > 0);
        }
        BENCHMARK_FINISH(cat, "getpgid", "Valid (Self PID=0)", iter);
    }

    // getpgid (Invalid: non-existent PID)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            pid_t pg = syscall(SYS_getpgid, 9999999);
            BENCHMARK_ITER_END(pg < 0 && errno == ESRCH);
        }
        BENCHMARK_FINISH(cat, "getpgid", "Invalid (ESRCH Non-existent PID)", iter);
    }

    // getsid (Valid: self)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            pid_t s = syscall(SYS_getsid, 0);
            BENCHMARK_ITER_END(s > 0);
        }
        BENCHMARK_FINISH(cat, "getsid", "Valid (Self PID=0)", iter);
    }

    // sched_yield
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_sched_yield);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "sched_yield", "Valid / Success Path", iter);
    }

    // sched_getparam (Valid)
    {
        struct sched_param sp;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_sched_getparam, 0, &sp);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "sched_getparam", "Valid (Self PID=0)", iter);
    }

    // sched_getscheduler (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_sched_getscheduler, 0);
            BENCHMARK_ITER_END(r >= 0);
        }
        BENCHMARK_FINISH(cat, "sched_getscheduler", "Valid (Self PID=0)", iter);
    }

    // prctl (Valid: PR_GET_DUMPABLE)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_prctl, PR_GET_DUMPABLE, 0, 0, 0, 0);
            BENCHMARK_ITER_END(r >= 0);
        }
        BENCHMARK_FINISH(cat, "prctl", "Valid (PR_GET_DUMPABLE)", iter);
    }

    // prctl (Invalid: invalid option)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_prctl, -1, 0, 0, 0, 0);
            BENCHMARK_ITER_END(r < 0 && errno == EINVAL);
        }
        BENCHMARK_FINISH(cat, "prctl", "Invalid (EINVAL option=-1)", iter);
    }

    // kill (Valid: sig 0 self check)
    {
        pid_t self = getpid();
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_kill, self, 0);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "kill", "Valid (Sig 0 Self Check)", iter);
    }

    // kill (Invalid: sig 0 non-existent PID)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_kill, 9999999, 0);
            BENCHMARK_ITER_END(r < 0 && errno == ESRCH);
        }
        BENCHMARK_FINISH(cat, "kill", "Invalid (ESRCH Non-existent PID)", iter);
    }

    // tgkill (Valid: sig 0 self)
    {
        pid_t self_p = getpid();
        pid_t self_t = syscall(SYS_gettid);
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_tgkill, self_p, self_t, 0);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "tgkill", "Valid (Sig 0 Self Thread Check)", iter);
    }

    // fork + exit / wait4
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            pid_t child = fork();
            if (child == 0) {
                _exit(0);
            } else if (child > 0) {
                int status = 0;
                syscall(SYS_wait4, child, &status, 0, NULL);
                BENCHMARK_ITER_END(WIFEXITED(status));
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "fork+wait4", "Valid (Subprocess Lifecycle)", iter);
    }
}

/* =========================================================================
 * Category 2: Memory Management Syscalls
 * ========================================================================= */
static void bench_memory(unsigned iter) {
    const char *cat = "Memory";

    // brk (Valid: Query current break)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            uintptr_t b = (uintptr_t)syscall(SYS_brk, 0);
            BENCHMARK_ITER_END(b != 0);
        }
        BENCHMARK_FINISH(cat, "brk", "Valid (Query Break Address)", iter);
    }

    // mmap 4KB Anonymous (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            void *p = (void*)syscall(SYS_mmap, NULL, 4096, PROT_READ|PROT_WRITE,
                                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) {
                syscall(SYS_munmap, p, 4096);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "mmap+munmap", "Valid (4KB Anon Private)", iter);
    }

    // mmap 64KB Anonymous (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            void *p = (void*)syscall(SYS_mmap, NULL, 65536, PROT_READ|PROT_WRITE,
                                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) {
                syscall(SYS_munmap, p, 65536);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "mmap+munmap", "Valid (64KB Anon Private)", iter);
    }

    // mmap (Invalid: Invalid Flags)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            void *p = (void*)syscall(SYS_mmap, NULL, 4096, -1,
                                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            BENCHMARK_ITER_END(p == MAP_FAILED && errno == EINVAL);
        }
        BENCHMARK_FINISH(cat, "mmap", "Invalid (EINVAL Invalid Prot -1)", iter);
    }

    // mprotect (Valid)
    {
        void *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                int r = syscall(SYS_mprotect, p, 4096, PROT_READ|PROT_WRITE);
                BENCHMARK_ITER_END(r == 0);
            }
            BENCHMARK_FINISH(cat, "mprotect", "Valid (PROT_READ -> PROT_READ|WRITE)", iter);
            munmap(p, 4096);
        }
    }

    // mprotect (Invalid: Null Pointer)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_mprotect, NULL, 4096, PROT_READ);
            BENCHMARK_ITER_END(r < 0 && (errno == ENOMEM || errno == EINVAL || errno == EFAULT));
        }
        BENCHMARK_FINISH(cat, "mprotect", "Invalid (EFAULT/ENOMEM Null Address)", iter);
    }

    // madvise (Valid: MADV_DONTNEED)
    {
        void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            memset(p, 0xAA, 4096);
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                int r = syscall(SYS_madvise, p, 4096, MADV_DONTNEED);
                BENCHMARK_ITER_END(r == 0);
            }
            BENCHMARK_FINISH(cat, "madvise", "Valid (MADV_DONTNEED 4KB)", iter);
            munmap(p, 4096);
        }
    }

    // madvise (Valid: MADV_WILLNEED)
    {
        void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                int r = syscall(SYS_madvise, p, 4096, MADV_WILLNEED);
                BENCHMARK_ITER_END(r == 0);
            }
            BENCHMARK_FINISH(cat, "madvise", "Valid (MADV_WILLNEED 4KB)", iter);
            munmap(p, 4096);
        }
    }

    // mlock & munlock (Valid)
    {
        void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                int r1 = syscall(SYS_mlock, p, 4096);
                int r2 = syscall(SYS_munlock, p, 4096);
                BENCHMARK_ITER_END(r1 == 0 && r2 == 0);
            }
            BENCHMARK_FINISH(cat, "mlock+munlock", "Valid (4KB Lock/Unlock)", iter);
            munmap(p, 4096);
        }
    }

#ifdef SYS_memfd_create
    // memfd_create (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int fd = syscall(SYS_memfd_create, "bench_memfd", 0);
            if (fd >= 0) {
                close(fd);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "memfd_create", "Valid / Success Path", iter);
    }
#endif
}

/* =========================================================================
 * Category 3: File & File Descriptor I/O Syscalls
 * ========================================================================= */
static void bench_file_io(unsigned iter) {
    const char *cat = "File I/O";

    // Setup temporary files
    char tmp_file[] = "/tmp/bench_file_XXXXXX";
    int tmp_fd = mkstemp(tmp_file);
    if (tmp_fd < 0) {
        perror("mkstemp");
        return;
    }
    // Write 64KB dummy data into tmp_fd
    char dummy_buf[65536];
    memset(dummy_buf, 'A', sizeof(dummy_buf));
    write(tmp_fd, dummy_buf, sizeof(dummy_buf));

    // open / openat (Valid: O_RDONLY)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int fd = syscall(SYS_openat, AT_FDCWD, tmp_file, O_RDONLY, 0);
            if (fd >= 0) {
                close(fd);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "openat", "Valid (O_RDONLY Existing File)", iter);
    }

    // openat (Invalid: Non-existent file)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int fd = syscall(SYS_openat, AT_FDCWD, "/non_existent_path_xyz123", O_RDONLY, 0);
            BENCHMARK_ITER_END(fd < 0 && errno == ENOENT);
        }
        BENCHMARK_FINISH(cat, "openat", "Invalid (ENOENT Non-existent File)", iter);
    }

    // close (Invalid: Invalid FD -1)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_close, -1);
            BENCHMARK_ITER_END(r < 0 && errno == EBADF);
        }
        BENCHMARK_FINISH(cat, "close", "Invalid (EBADF FD=-1)", iter);
    }

    // read 0 bytes from /dev/zero
    int dev_zero = open("/dev/zero", O_RDONLY);
    int dev_null = open("/dev/null", O_WRONLY);

    if (dev_zero >= 0) {
        // read 0 bytes
        {
            char buf[16];
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t r = syscall(SYS_read, dev_zero, buf, 0);
                BENCHMARK_ITER_END(r == 0);
            }
            BENCHMARK_FINISH(cat, "read", "Valid (0-byte payload)", iter);
        }
        // read 16 bytes
        {
            char buf[16];
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t r = syscall(SYS_read, dev_zero, buf, 16);
                BENCHMARK_ITER_END(r == 16);
            }
            BENCHMARK_FINISH(cat, "read", "Valid (16-byte payload)", iter);
        }
        // read 4096 bytes
        {
            char buf[4096];
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t r = syscall(SYS_read, dev_zero, buf, 4096);
                BENCHMARK_ITER_END(r == 4096);
            }
            BENCHMARK_FINISH(cat, "read", "Valid (4096-byte payload)", iter);
        }
        // read 65536 bytes
        {
            char *buf = malloc(65536);
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t r = syscall(SYS_read, dev_zero, buf, 65536);
                BENCHMARK_ITER_END(r == 65536);
            }
            BENCHMARK_FINISH(cat, "read", "Valid (65536-byte payload)", iter);
            free(buf);
        }
    }

    // read (Invalid: EBADF)
    {
        char buf[16];
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            ssize_t r = syscall(SYS_read, -1, buf, 16);
            BENCHMARK_ITER_END(r < 0 && errno == EBADF);
        }
        BENCHMARK_FINISH(cat, "read", "Invalid (EBADF FD=-1)", iter);
    }

    if (dev_null >= 0) {
        // write 0 bytes
        {
            char buf[16];
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t r = syscall(SYS_write, dev_null, buf, 0);
                BENCHMARK_ITER_END(r == 0);
            }
            BENCHMARK_FINISH(cat, "write", "Valid (0-byte payload)", iter);
        }
        // write 16 bytes
        {
            char buf[16];
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t r = syscall(SYS_write, dev_null, buf, 16);
                BENCHMARK_ITER_END(r == 16);
            }
            BENCHMARK_FINISH(cat, "write", "Valid (16-byte payload)", iter);
        }
        // write 4096 bytes
        {
            char buf[4096];
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t r = syscall(SYS_write, dev_null, buf, 4096);
                BENCHMARK_ITER_END(r == 4096);
            }
            BENCHMARK_FINISH(cat, "write", "Valid (4096-byte payload)", iter);
        }
        // write 65536 bytes
        {
            char *buf = malloc(65536);
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t r = syscall(SYS_write, dev_null, buf, 65536);
                BENCHMARK_ITER_END(r == 65536);
            }
            BENCHMARK_FINISH(cat, "write", "Valid (65536-byte payload)", iter);
            free(buf);
        }
    }

    // pread64 (Valid)
    {
        char buf[1024];
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            ssize_t r = syscall(SYS_pread64, tmp_fd, buf, 1024, 0);
            BENCHMARK_ITER_END(r == 1024);
        }
        BENCHMARK_FINISH(cat, "pread64", "Valid (1024 bytes offset 0)", iter);
    }

    // readv (Valid)
    {
        char b1[512], b2[512];
        struct iovec iov[2] = { {b1, 512}, {b2, 512} };
        syscall(SYS_lseek, tmp_fd, 0, SEEK_SET);
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            ssize_t r = syscall(SYS_readv, tmp_fd, iov, 2);
            BENCHMARK_ITER_END(r == 1024);
            syscall(SYS_lseek, tmp_fd, 0, SEEK_SET);
        }
        BENCHMARK_FINISH(cat, "readv", "Valid (2-chunk iovec 1024B)", iter);
    }

    // lseek (Valid: SEEK_SET)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            off_t o = syscall(SYS_lseek, tmp_fd, 0, SEEK_SET);
            BENCHMARK_ITER_END(o == 0);
        }
        BENCHMARK_FINISH(cat, "lseek", "Valid (SEEK_SET Offset 0)", iter);
    }

    // dup & dup2 (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int newfd = syscall(SYS_dup, tmp_fd);
            if (newfd >= 0) {
                close(newfd);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "dup", "Valid / Success Path", iter);
    }

    // fcntl (Valid: F_GETFL)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int fl = syscall(SYS_fcntl, tmp_fd, F_GETFL, 0);
            BENCHMARK_ITER_END(fl >= 0);
        }
        BENCHMARK_FINISH(cat, "fcntl", "Valid (F_GETFL Flags Query)", iter);
    }

    // flock (Valid: LOCK_SH / LOCK_UN)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r1 = syscall(SYS_flock, tmp_fd, LOCK_SH);
            int r2 = syscall(SYS_flock, tmp_fd, LOCK_UN);
            BENCHMARK_ITER_END(r1 == 0 && r2 == 0);
        }
        BENCHMARK_FINISH(cat, "flock", "Valid (LOCK_SH + LOCK_UN)", iter);
    }

    // fsync & fdatasync
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_fsync, tmp_fd);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "fsync", "Valid / Success Path", iter);
    }

    // fstat / newfstatat (Valid)
    {
        struct stat st;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_fstat, tmp_fd, &st);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "fstat", "Valid / Success Path", iter);
    }

    // newfstatat (Valid)
    {
        struct stat st;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_newfstatat, AT_FDCWD, tmp_file, &st, 0);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "newfstatat", "Valid (Path Stat Query)", iter);
    }

    // access / faccessat2
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_access, tmp_file, F_OK);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "access", "Valid (F_OK Existing File)", iter);
    }

    // chmod / fchmod
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_fchmod, tmp_fd, 0644);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "fchmod", "Valid (Mode 0644)", iter);
    }

    // umask
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            mode_t old_m = syscall(SYS_umask, 0022);
            BENCHMARK_ITER_END(old_m != (mode_t)-1);
        }
        BENCHMARK_FINISH(cat, "umask", "Valid / Success Path", iter);
    }

    // getcwd
    {
        char cwd[4096];
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            long r = syscall(SYS_getcwd, cwd, sizeof(cwd));
            BENCHMARK_ITER_END(r > 0);
        }
        BENCHMARK_FINISH(cat, "getcwd", "Valid (4096-byte Buffer)", iter);
    }

    // Cleanup temporary files & FDs
    if (dev_zero >= 0) close(dev_zero);
    if (dev_null >= 0) close(dev_null);
    close(tmp_fd);
    unlink(tmp_file);
}

/* =========================================================================
 * Category 4: Time & Timer Syscalls
 * ========================================================================= */
static void bench_time_timers(unsigned iter) {
    const char *cat = "Time/Timers";

    // gettimeofday (Valid)
    {
        struct timeval tv;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_gettimeofday, &tv, NULL);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "gettimeofday", "Valid / Success Path", iter);
    }

    // clock_gettime (CLOCK_MONOTONIC)
    {
        struct timespec ts;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "clock_gettime", "Valid (CLOCK_MONOTONIC)", iter);
    }

    // clock_gettime (CLOCK_REALTIME)
    {
        struct timespec ts;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "clock_gettime", "Valid (CLOCK_REALTIME)", iter);
    }

    // clock_gettime (CLOCK_THREAD_CPUTIME_ID)
    {
        struct timespec ts;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_clock_gettime, CLOCK_THREAD_CPUTIME_ID, &ts);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "clock_gettime", "Valid (CLOCK_THREAD_CPUTIME_ID)", iter);
    }

    // clock_gettime (Invalid: Clock ID -1)
    {
        struct timespec ts;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_clock_gettime, -1, &ts);
            BENCHMARK_ITER_END(r < 0 && errno == EINVAL);
        }
        BENCHMARK_FINISH(cat, "clock_gettime", "Invalid (EINVAL Clock ID -1)", iter);
    }

    // clock_getres (Valid: CLOCK_MONOTONIC)
    {
        struct timespec ts;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_clock_getres, CLOCK_MONOTONIC, &ts);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "clock_getres", "Valid (CLOCK_MONOTONIC Resolution)", iter);
    }

    // nanosleep (Valid: 0s 0ns)
    {
        struct timespec req = {0, 0}, rem;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_nanosleep, &req, &rem);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "nanosleep", "Valid (0ns Duration)", iter);
    }

    // alarm (Valid: 0)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            unsigned rem = syscall(SYS_alarm, 0);
            (void)rem;
            BENCHMARK_ITER_END(1);
        }
        BENCHMARK_FINISH(cat, "alarm", "Valid (Cancel Alarm = 0)", iter);
    }

    // setitimer (Valid: Query ITIMER_REAL)
    {
        struct itimerval itv = {{0, 0}, {0, 0}}, old_itv;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_setitimer, ITIMER_REAL, &itv, &old_itv);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "setitimer", "Valid (Query/Disable ITIMER_REAL)", iter);
    }

    // timerfd_create (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int tfd = syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_NONBLOCK);
            if (tfd >= 0) {
                close(tfd);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "timerfd_create", "Valid (CLOCK_MONOTONIC TFD_NONBLOCK)", iter);
    }
}

/* =========================================================================
 * Category 5: Signal Handling Syscalls
 * ========================================================================= */
static void bench_signals(unsigned iter) {
    const char *cat = "Signals";

    // rt_sigaction (Valid: Query SIGUSR1)
    {
        struct sigaction sa;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_rt_sigaction, SIGUSR1, NULL, &sa, 8);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "rt_sigaction", "Valid (Query SIGUSR1 Handler)", iter);
    }

    // rt_sigaction (Invalid: Signal 0)
    {
        struct sigaction sa;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_rt_sigaction, 0, NULL, &sa, 8);
            BENCHMARK_ITER_END(r < 0 && errno == EINVAL);
        }
        BENCHMARK_FINISH(cat, "rt_sigaction", "Invalid (EINVAL Signal 0)", iter);
    }

    // rt_sigprocmask (Valid: SIG_BLOCK empty set -> SIG_UNBLOCK)
    {
        sigset_t set, oldset;
        sigemptyset(&set);
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_rt_sigprocmask, SIG_BLOCK, &set, &oldset, 8);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "rt_sigprocmask", "Valid (SIG_BLOCK Empty Set)", iter);
    }

    // sigaltstack (Valid: Query current stack)
    {
        stack_t old_ss;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_sigaltstack, NULL, &old_ss);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "sigaltstack", "Valid (Query Current Alt Stack)", iter);
    }
}

/* =========================================================================
 * Category 6: I/O Multiplexing & Event Syscalls
 * ========================================================================= */
static void bench_events_epoll(unsigned iter) {
    const char *cat = "I/O Events";

    // poll (Valid: 0 fds, timeout 0)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_poll, NULL, 0, 0);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "poll", "Valid (0 FDs, Timeout 0ms)", iter);
    }

    // poll (Valid: 1 fd /dev/null, timeout 0)
    {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            struct pollfd pfd = { fd, POLLOUT, 0 };
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                int r = syscall(SYS_poll, &pfd, 1, 0);
                BENCHMARK_ITER_END(r == 1);
            }
            BENCHMARK_FINISH(cat, "poll", "Valid (1 FD POLLOUT, Timeout 0ms)", iter);
            close(fd);
        }
    }

    // select (Valid: 0 fds, timeout 0)
    {
        struct timeval tv = {0, 0};
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            tv.tv_sec = 0; tv.tv_usec = 0;
            int r = syscall(SYS_select, 0, NULL, NULL, NULL, &tv);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "select", "Valid (0 FDs, Timeout 0ms)", iter);
    }

    // epoll_create1 (Valid)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int epfd = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
            if (epfd >= 0) {
                close(epfd);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "epoll_create1", "Valid (EPOLL_CLOEXEC)", iter);
    }

    // epoll_ctl (Valid: EPOLL_CTL_ADD / DEL)
    {
        int epfd = epoll_create1(0);
        int dummy_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (epfd >= 0 && dummy_fd >= 0) {
            struct epoll_event ev = { EPOLLOUT, {.fd = dummy_fd} };
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                int r1 = syscall(SYS_epoll_ctl, epfd, EPOLL_CTL_ADD, dummy_fd, &ev);
                int r2 = syscall(SYS_epoll_ctl, epfd, EPOLL_CTL_DEL, dummy_fd, NULL);
                BENCHMARK_ITER_END(r1 == 0 && r2 == 0);
            }
            BENCHMARK_FINISH(cat, "epoll_ctl", "Valid (EPOLL_CTL_ADD + DEL)", iter);
            close(epfd);
            close(dummy_fd);
        }
    }

    // epoll_wait (Valid: Empty epoll set, timeout 0)
    {
        int epfd = epoll_create1(0);
        if (epfd >= 0) {
            struct epoll_event events[4];
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                int r = syscall(SYS_epoll_wait, epfd, events, 4, 0);
                BENCHMARK_ITER_END(r == 0);
            }
            BENCHMARK_FINISH(cat, "epoll_wait", "Valid (Empty Set, Timeout 0ms)", iter);
            close(epfd);
        }
    }

    // eventfd2 (Valid: Create, Write, Read, Close)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int efd = syscall(SYS_eventfd2, 0, EFD_NONBLOCK);
            if (efd >= 0) {
                uint64_t val = 1;
                syscall(SYS_write, efd, &val, sizeof(val));
                syscall(SYS_read, efd, &val, sizeof(val));
                close(efd);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "eventfd2", "Valid (Create+Write+Read+Close)", iter);
    }
}

/* =========================================================================
 * Category 7: Sockets & IPC Syscalls
 * ========================================================================= */
static void bench_sockets_ipc(unsigned iter) {
    const char *cat = "Sockets/IPC";

    // pipe / pipe2 (Valid)
    {
        int fds[2];
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_pipe2, fds, O_CLOEXEC);
            if (r == 0) {
                close(fds[0]);
                close(fds[1]);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "pipe2", "Valid (Create & Close Pipe)", iter);
    }

    // pipe read/write 64-byte payload
    {
        int fds[2];
        if (pipe(fds) == 0) {
            char buf[64] = "Benchmarking pipe IPC speed throughput test data payload!";
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t w = syscall(SYS_write, fds[1], buf, 64);
                ssize_t r = syscall(SYS_read, fds[0], buf, 64);
                BENCHMARK_ITER_END(w == 64 && r == 64);
            }
            BENCHMARK_FINISH(cat, "pipe_write+read", "Valid (64-byte IPC Ping-Pong)", iter);
            close(fds[0]);
            close(fds[1]);
        }
    }

    // socket (AF_UNIX, SOCK_STREAM)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int s = syscall(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
            if (s >= 0) {
                close(s);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "socket", "Valid (AF_UNIX SOCK_STREAM)", iter);
    }

    // socket (AF_INET, SOCK_DGRAM)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int s = syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0);
            if (s >= 0) {
                close(s);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "socket", "Valid (AF_INET SOCK_DGRAM)", iter);
    }

    // socket (Invalid: Domain -1)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int s = syscall(SYS_socket, -1, SOCK_STREAM, 0);
            BENCHMARK_ITER_END(s < 0 && (errno == EAFNOSUPPORT || errno == EINVAL));
        }
        BENCHMARK_FINISH(cat, "socket", "Invalid (EAFNOSUPPORT Domain -1)", iter);
    }

    // socketpair (AF_UNIX)
    {
        int sv[2];
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sv);
            if (r == 0) {
                close(sv[0]);
                close(sv[1]);
                BENCHMARK_ITER_END(1);
            } else {
                BENCHMARK_ITER_END(0);
            }
        }
        BENCHMARK_FINISH(cat, "socketpair", "Valid (AF_UNIX Connected Pair)", iter);
    }

    // sendto + recvfrom on socketpair
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
            char send_buf[64] = "Datagram socketpair speed benchmark payload data!";
            char recv_buf[64];
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                ssize_t s = syscall(SYS_sendto, sv[0], send_buf, 64, 0, NULL, 0);
                ssize_t r = syscall(SYS_recvfrom, sv[1], recv_buf, 64, 0, NULL, NULL);
                BENCHMARK_ITER_END(s == 64 && r == 64);
            }
            BENCHMARK_FINISH(cat, "sendto+recvfrom", "Valid (64-byte Datagram Transfer)", iter);
            close(sv[0]);
            close(sv[1]);
        }
    }

    // getsockname / getpeername
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            struct sockaddr_un addr;
            socklen_t len = sizeof(addr);
            BENCHMARK_START();
            for (unsigned i = 0; i < iter; i++) {
                BENCHMARK_ITER_START();
                int r1 = syscall(SYS_getsockname, sv[0], (struct sockaddr*)&addr, &len);
                int r2 = syscall(SYS_getpeername, sv[0], (struct sockaddr*)&addr, &len);
                BENCHMARK_ITER_END(r1 == 0 && r2 == 0);
            }
            BENCHMARK_FINISH(cat, "getsockname+peername", "Valid (Socket Address Query)", iter);
            close(sv[0]);
            close(sv[1]);
        }
    }

    // futex (FUTEX_WAKE with 0 waiters)
    {
        int futex_val = 0;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_futex, &futex_val, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
            BENCHMARK_ITER_END(r >= 0);
        }
        BENCHMARK_FINISH(cat, "futex", "Valid (FUTEX_WAKE_PRIVATE 0 Waiters)", iter);
    }
}

/* =========================================================================
 * Category 8: System Info & Random Syscalls
 * ========================================================================= */
static void bench_system_info(unsigned iter) {
    const char *cat = "System/Info";

    // uname (Valid)
    {
        struct utsname uts;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_uname, &uts);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "uname", "Valid / Success Path", iter);
    }

    // uname (Invalid: NULL pointer)
    {
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_uname, NULL);
            BENCHMARK_ITER_END(r < 0 && errno == EFAULT);
        }
        BENCHMARK_FINISH(cat, "uname", "Invalid (EFAULT Null Address)", iter);
    }

    // sysinfo (Valid)
    {
        struct sysinfo si;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            int r = syscall(SYS_sysinfo, &si);
            BENCHMARK_ITER_END(r == 0);
        }
        BENCHMARK_FINISH(cat, "sysinfo", "Valid / Success Path", iter);
    }

#ifdef SYS_getrandom
    // getrandom (Valid: GRND_NONBLOCK 8 bytes)
    {
        uint64_t rand_val;
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            ssize_t r = syscall(SYS_getrandom, &rand_val, sizeof(rand_val), GRND_NONBLOCK);
            BENCHMARK_ITER_END(r == (ssize_t)sizeof(rand_val));
        }
        BENCHMARK_FINISH(cat, "getrandom", "Valid (8-byte Non-blocking)", iter);
    }

    // getrandom (Valid: GRND_NONBLOCK 256 bytes)
    {
        char buf[256];
        BENCHMARK_START();
        for (unsigned i = 0; i < iter; i++) {
            BENCHMARK_ITER_START();
            ssize_t r = syscall(SYS_getrandom, buf, sizeof(buf), GRND_NONBLOCK);
            BENCHMARK_ITER_END(r == (ssize_t)sizeof(buf));
        }
        BENCHMARK_FINISH(cat, "getrandom", "Valid (256-byte Non-blocking)", iter);
    }
#endif
}

/* =========================================================================
 * Formatted Report Printer
 * ========================================================================= */
static void print_report(unsigned iterations) {
    printf("\n====================================================================================================\n");
    printf("                    ASCENTOS / LINUX SYSCALL SPEED BENCHMARK REPORT (%u ITERATIONS EACH)\n", iterations);
    printf("====================================================================================================\n");
    printf("%-16s | %-20s | %-32s | %-9s | %-9s | %-12s | %-7s\n",
           "Category", "Syscall", "Situation / Scenario", "Avg (ns)", "Min (ns)", "Throughput", "Pass/Err");
    printf("-----------------+----------------------+----------------------------------+-----------+-----------+--------------+---------\n");

    double grand_total_ns = 0.0;
    unsigned total_tests = num_results;

    for (size_t i = 0; i < num_results; i++) {
        bench_result_t *r = &results[i];
        grand_total_ns += (double)r->total_ns;
        char ops_str[32];
        if (r->ops_per_sec >= 1e6) {
            snprintf(ops_str, sizeof(ops_str), "%.2f M/s", r->ops_per_sec / 1e6);
        } else {
            snprintf(ops_str, sizeof(ops_str), "%.2f K/s", r->ops_per_sec / 1e3);
        }

        char pass_str[16];
        snprintf(pass_str, sizeof(pass_str), "%d/%d", r->success_count, r->error_count);

        printf("%-16s | %-20s | %-32s | %9.1f | %9lu | %12s | %-7s\n",
               r->category, r->syscall_name, r->situation,
               r->avg_ns, (unsigned long)r->min_ns, ops_str, pass_str);
    }

    printf("====================================================================================================\n");
    printf("SUMMARY: Benchmark completed %u scenarios across %u iterations each (Total Syscall Invocations: %lu)\n",
           total_tests, iterations, (unsigned long)total_tests * iterations);
    printf("Total Execution Time: %.2f ms\n", grand_total_ns / 1e6);
    printf("====================================================================================================\n\n");
}

int main(int argc, char **argv) {
    unsigned iterations = DEFAULT_ITERATIONS;
    if (argc > 1) {
        int user_iter = atoi(argv[1]);
        if (user_iter > 0) iterations = (unsigned)user_iter;
    }

    printf("Starting Syscall Speed Benchmark Suite (%u iterations per scenario)...\n", iterations);
    fflush(stdout);

    bench_process_thread(iterations);
    bench_memory(iterations);
    bench_file_io(iterations);
    bench_time_timers(iterations);
    bench_signals(iterations);
    bench_events_epoll(iterations);
    bench_sockets_ipc(iterations);
    bench_system_info(iterations);

    print_report(iterations);

    return 0;
}
