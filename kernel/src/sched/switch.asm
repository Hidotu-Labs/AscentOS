global switch_context
global thread_stub

extern fpu_on_context_switch

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

    ; ---- FPU state is NOT saved here (Lazy FPU) ----
    ; The fxsave64 is now deferred: CR0.TS is set below so that the
    ; arriving thread takes a #NM fault on its first FPU/SSE instruction,
    ; triggering fpu_nm_handler which saves the old owner and loads the new
    ; owner's state just-in-time.  Threads that never use FPU never pay.

    ; Save current stack pointer into old_t->rsp (offset 0)
    mov [rdi], rsp

    ; Load new stack pointer from new_t->rsp (offset 0)
    mov rsp, [rsi]

    ; The old kernel stack is no longer in use. Clear this actual CPU's
    ; stack hazard here; a resumed C frame may hold a stale CPU pointer.
    mov qword [gs:376], 0

    ; Set CR0.TS = 1 to mark FPU state as stale for the incoming thread.
    ; The arriving thread will take #NM on its first SSE/x87 instruction,
    ; at which point fpu_nm_handler will do the actual fxsave/fxrstor.
    mov rax, cr0
    or  rax, 8          ; CR0.TS bit (bit 3)
    mov cr0, rax

    ; ---- FPU state is NOT restored here (Lazy FPU) ----

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
