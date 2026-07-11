.text
.global ascent_clone
.type ascent_clone,@function
/* long ascent_clone(int (*fn)(void *), void *stack, unsigned long flags,
 *                   void *arg, int *ctid);
 */
ascent_clone:
    andq $-16, %rsi
    subq $16, %rsi
    movq %rdi, 0(%rsi)
    movq %rcx, 8(%rsi)

    movq %rdx, %rdi
    xorq %rdx, %rdx
    movq %r8, %r10
    xorq %r8, %r8
    movq $56, %rax
    syscall

    testq %rax, %rax
    jnz 1f

    xorl %ebp, %ebp
    popq %rax
    popq %rdi
    call *%rax
    movl %eax, %edi
    movq $60, %rax
    syscall
    ud2
1:
    ret
.size ascent_clone, .-ascent_clone
.section .note.GNU-stack,"",@progbits
