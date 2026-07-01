global syscall_entry
extern syscall_dispatcher
extern console_puts

section .text

syscall_entry:

    swapgs

    mov gs:[336], rsp

    mov rsp, gs:[24]

    push qword gs:[336] ; User RSP
    push r11           ; User RFLAGS
    push rcx           ; User RIP

    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx
    push rax
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi

    mov rbp, rsp
    and rsp, -16

    mov rdi, rbp
    call syscall_dispatcher

    mov rsp, rbp

    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    pop rax
    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r14
    pop r15

    pop rcx ; User RIP
    pop r11 ; User RFLAGS
    pop rsp ; User RSP

    ; Swap GS back to User TLS (if any)
    ; Disable interrupts to protect the window between swapgs and sysret,
    ; as the ISR would now see a kernel CS but with user GS.
    cli
    swapgs

    ; Return to user mode safely
    o64 sysret
