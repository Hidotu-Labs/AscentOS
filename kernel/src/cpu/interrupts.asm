extern isr_handler

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push %1
    jmp isr_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

; IRQs
ISR_NOERRCODE 32
ISR_NOERRCODE 33
ISR_NOERRCODE 34
ISR_NOERRCODE 35
ISR_NOERRCODE 36
ISR_NOERRCODE 37
ISR_NOERRCODE 38
ISR_NOERRCODE 39
ISR_NOERRCODE 40
ISR_NOERRCODE 41
ISR_NOERRCODE 42
ISR_NOERRCODE 43
ISR_NOERRCODE 44
ISR_NOERRCODE 45
ISR_NOERRCODE 46
ISR_NOERRCODE 47

; LAPIC timer interrupt vector
ISR_NOERRCODE 48

; Reschedule IPI vector
ISR_NOERRCODE 49

; LAPIC spurious interrupt vector
ISR_NOERRCODE 255

isr_common_stub:
    ; Hardware has already switched RSP to the kernel stack (via TSS) if coming from Ring 3.
    ; First, check if we came from user mode (Ring 3) to decide if swapgs is needed.
    ; Interrupt frame on stack: SS, RSP, RFLAGS, CS, RIP, ERR, INT
    ; RSP points to INT. CS is at [RSP + 24].
    test qword [rsp + 24], 3
    jz .skip_swapgs
    swapgs
.skip_swapgs:

    ; Push all general purpose registers (matching struct registers)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Call C handler
    mov rdi, rsp           ; First parameter: pointer to struct registers
    mov rbp, rsp           ; Save original RSP
    and rsp, -16           ; Align stack to 16 bytes for System V AMD64 ABI
    call isr_handler
    mov rsp, rbp           ; Restore original RSP

    ; Pop all general purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Swap GS back if we came from user mode
    test qword [rsp + 24], 3
    jz .skip_swapgs_exit
    swapgs
.skip_swapgs_exit:

    add rsp, 16 ; remove error code and int number
    iretq
