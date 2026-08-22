global switch_context
global thread_stub

section .text

; void switch_context(struct thread *old_t, struct thread *new_t)
; rdi = pointer to old thread struct
; rsi = pointer to new thread struct
switch_context:
    ; Push callee-saved registers according to System V AMD64 ABI
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Save FPU state (at offset 16 in struct thread)
    fxsave64 [rdi + 16]

    ; Save current stack pointer into old_t->rsp (offset 0)
    mov [rdi], rsp

    ; Load new stack pointer from new_t->rsp (offset 0)
    mov rsp, [rsi]

    ; The old kernel stack is no longer in use. Clear this actual CPU's
    ; stack hazard here; a resumed C frame may hold a stale CPU pointer.
    mov qword [gs:376], 0

    ; Restore FPU state from new thread (at offset 16 in struct thread)
    fxrstor64 [rsi + 16]

    ; Pop callee-saved registers for the arriving thread
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Return to the address left on the new thread's stack
    ret

; This stub is where newly created threads begin execution.
; The switch_context "ret" instruction pops into here.
; 'r12' contains the actual C function entry point (set in sched_create_kernel_thread).
thread_stub:
    ; Ensure interrupts are enabled for the new thread
    sti

    ; Call the entry function
    call r12

    ; If the entry function returns, it will return into thread_exit()
    ; because thread_exit was pushed just below the context struct.
    ret
