.text
.global proc_bench_clone
.type proc_bench_clone, @function

/*
 * long proc_bench_clone(int (*fn)(void *), void *stack,
 *                       unsigned long flags, void *arg,
 *                       int *child_tid);
 *
 * x86-64 SysV calling convention on entry:
 *   rdi = fn
 *   rsi = stack   (top of the allocated region)
 *   rdx = flags
 *   rcx = arg
 *   r8  = child_tid ptr (CLONE_CHILD_SETTID / CLONE_CHILD_CLEARTID)
 *
 * clone(2) Linux ABI (syscall 56):
 *   rdi = flags
 *   rsi = child_stack
 *   rdx = ptid  (unused here, 0)
 *   r10 = ctid  (child_tid ptr)
 *   r8  = tls   (unused here, 0)
 *
 * We push fn + arg onto the child stack so the child can pop them
 * without needing a global variable (safe for concurrent iterations).
 */
proc_bench_clone:
    /* Align child stack to 16 bytes, then carve two slots. */
    andq    $-16, %rsi
    subq    $16,  %rsi
    movq    %rdi, 0(%rsi)   /* slot 0: fn  */
    movq    %rcx, 8(%rsi)   /* slot 1: arg */

    /* Set up clone(2) arguments. */
    movq    %rdx, %rdi      /* flags       */
    /* rsi already = child_stack */
    xorq    %rdx, %rdx      /* ptid = NULL */
    movq    %r8,  %r10      /* ctid        */
    xorq    %r8,  %r8       /* tls  = NULL */
    movq    $56,  %rax      /* SYS_clone   */
    syscall

    /* Parent path: rax = child tid (> 0) or error (< 0). */
    testq   %rax, %rax
    jnz     1f

    /*
     * Child path: rax == 0.
     * Stack now looks like: [fn, arg] (we pushed them above).
     * Clear the frame pointer for clean backtraces.
     */
    xorl    %ebp, %ebp
    popq    %rax            /* fn  */
    popq    %rdi            /* arg */
    call    *%rax

    /* Child exits via SYS_exit (60) with fn's return value. */
    movl    %eax, %edi
    movq    $60,  %rax
    syscall
    ud2                     /* unreachable */
1:
    ret
.size proc_bench_clone, .-proc_bench_clone

/* Mark stack as non-executable. */
.section .note.GNU-stack, "", @progbits
