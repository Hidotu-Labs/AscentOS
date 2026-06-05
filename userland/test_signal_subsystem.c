#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sched.h>
#include <errno.h>
#include <stdlib.h>
#include <stdatomic.h>

// AscentOS specific syscall numbers
#define AS_SYS_RT_SIGACTION 13
#define AS_SYS_RT_SIGPROCMASK 14
#define AS_SYS_RT_SIGRETURN 15
#define AS_SYS_GETPID 39
#define AS_SYS_CLONE 56
#define AS_SYS_EXIT 60
#define AS_SYS_SIGALTSTACK 131
#define AS_SYS_TGKILL 234

// Signal constants from kernel
#define AS_SIGUSR1 10
#define AS_SIGUSR2 12
#define AS_SA_ONSTACK 0x08000000
#define AS_SA_RESTORER 0x04000000

#define AS_ALT_STACK_SIZE 16384

typedef struct {
    uint64_t as_ss_sp;
    uint32_t as_ss_flags;
    uint32_t as_pad;
    uint64_t as_ss_size;
} as_stack_t;

struct as_sigaction {
    void (*as_handler)(int);
    uint64_t as_flags;
    void (*as_restorer)(void);
    uint64_t as_mask;
};

// Global state for verification
static _Atomic int handler_called = 0;
static _Atomic uint64_t handler_rsp = 0;
static uint8_t alt_stack[AS_ALT_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t thread_stack[AS_ALT_STACK_SIZE] __attribute__((aligned(16)));

// Restorer function to return from signal handler
void as_signal_restorer(void) {
    __asm__ volatile (
        "movq $15, %%rax\n" // SYS_RT_SIGRETURN
        "syscall\n"
        : : : "rax", "memory"
    );
}

// Direct syscall wrappers
static long raw_sigaltstack(const as_stack_t *ss, as_stack_t *oss) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rdi\n"
        "movq %2, %%rsi\n"
        "movq $131, %%rax\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r" (ret)
        : "r" (ss), "r" (oss)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    return ret;
}

static long raw_rt_sigaction(int sig, const struct as_sigaction *act, struct as_sigaction *oact) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rdi\n"
        "movq %2, %%rsi\n"
        "movq %3, %%rdx\n"
        "movq $8, %%r10\n" // sigsetsize
        "movq $13, %%rax\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r" (ret)
        : "r" ((long)sig), "r" (act), "r" (oact)
        : "rax", "rdi", "rsi", "rdx", "r10", "rcx", "r11", "memory"
    );
    return ret;
}

static long raw_tgkill(int tgid, int tid, int sig) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rdi\n"
        "movq %2, %%rsi\n"
        "movq %3, %%rdx\n"
        "movq $234, %%rax\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r" (ret)
        : "r" ((long)tgid), "r" ((long)tid), "r" ((long)sig)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static long raw_getpid() {
    long ret;
    __asm__ volatile(
        "movq $39, %%rax\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r" (ret)
        : 
        : "rax", "rcx", "r11", "memory"
    );
    return ret;
}

static long raw_clone(unsigned long flags, void *child_stack) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rdi\n"
        "movq %2, %%rsi\n"
        "movq $56, %%rax\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r" (ret)
        : "r" (flags), "r" (child_stack)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory"
    );
    return ret;
}

// Signal handler
void test_handler(int sig) {
    (void)sig;
    uint64_t sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    // We can't safely call printf here if we want to be 100% sure about SP,
    // but the kernel log should show we reached it.
    atomic_store(&handler_rsp, sp);
    atomic_store(&handler_called, 1);
}

// Thread state
static _Atomic int thread_received_signal = 0;

void thread_signal_handler(int sig) {
    (void)sig;
    atomic_store(&thread_received_signal, 1);
}

int thread_main(void *arg) {
    (void)arg;
    while (atomic_load(&thread_received_signal) == 0) {
        __asm__ volatile("pause");
    }
    syscall(AS_SYS_EXIT, 0);
    return 0;
}

int main() {
    printf("=== AscentOS Signal Subsystem Test (sigaltstack & tgkill) ===\n");

    // --- TEST 1: sigaltstack ---
    printf("\n--- Test 1: sigaltstack ---\n");
    
    // Register handler with SA_ONSTACK and SA_RESTORER
    struct as_sigaction sa = {0};
    sa.as_handler = test_handler;
    sa.as_flags = AS_SA_ONSTACK | AS_SA_RESTORER;
    sa.as_restorer = as_signal_restorer;
    if (raw_rt_sigaction(AS_SIGUSR1, &sa, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    // Set up alternate stack
    as_stack_t ss = {0};
    ss.as_ss_sp = (uintptr_t)alt_stack;
    ss.as_ss_size = sizeof(alt_stack);
    ss.as_ss_flags = 0;
    if (raw_sigaltstack(&ss, NULL) != 0) {
        perror("sigaltstack");
        return 1;
    }

    printf("[INFO] Alternate stack range: %p - %p\n", (void*)alt_stack, (void*)(alt_stack + sizeof(alt_stack)));

    // Trigger signal
    handler_called = 0;
    handler_rsp = 0;
    printf("[INFO] Triggering SIGUSR1...\n");
    kill(getpid(), AS_SIGUSR1);

    // Wait for handler
    int timeout = 1000;
    while (!atomic_load(&handler_called) && timeout-- > 0) usleep(1000);

    if (atomic_load(&handler_called)) {
        uintptr_t sp = (uintptr_t)atomic_load(&handler_rsp);
        printf("[PASS] Handler called, RSP: 0x%lx\n", sp);
        if (sp >= (uintptr_t)alt_stack && sp < (uintptr_t)(alt_stack + sizeof(alt_stack))) {
            printf("[PASS] RSP is within alternate stack range!\n");
        } else {
            printf("[FAIL] RSP is NOT within alternate stack range.\n");
        }
    } else {
        printf("[FAIL] Handler was not called (or crashed).\n");
    }

    // --- TEST 2: tgkill ---
    printf("\n--- Test 2: tgkill ---\n");

    int tgid = raw_getpid();
    printf("[INFO] My TGID: %d\n", tgid);

    // Register handler for thread
    struct as_sigaction sa_thread = {0};
    sa_thread.as_handler = thread_signal_handler;
    sa_thread.as_flags = AS_SA_RESTORER;
    sa_thread.as_restorer = as_signal_restorer;
    raw_rt_sigaction(AS_SIGUSR2, &sa_thread, NULL);

    // Clone a thread
    unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
    void *stack_top = thread_stack + sizeof(thread_stack);
    
    long thread_tid = raw_clone(flags, stack_top);
    if (thread_tid < 0) {
        perror("clone");
        return 1;
    }

    if (thread_tid == 0) {
        thread_main(NULL);
        _exit(0);
    }

    printf("[INFO] Spawned thread TID: %ld\n", thread_tid);
    usleep(100000); // Give it time to start

    printf("[INFO] Sending SIGUSR2 to thread %ld using tgkill(tgid=%d)...\n", thread_tid, tgid);
    long ret = raw_tgkill(tgid, (int)thread_tid, AS_SIGUSR2);
    if (ret != 0) {
        printf("[FAIL] tgkill returned error: %ld\n", ret);
    } else {
        printf("[INFO] tgkill success, waiting for thread...\n");
    }

    timeout = 1000;
    while (!atomic_load(&thread_received_signal) && timeout-- > 0) usleep(1000);

    if (atomic_load(&thread_received_signal)) {
        printf("[PASS] Thread received the signal via tgkill!\n");
    } else {
        printf("[FAIL] Thread did not receive the signal.\n");
    }

    printf("\n=== All tests completed ===\n");
    return 0;
}
