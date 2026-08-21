section .rodata
global vdso_blob_start
global vdso_blob_end

align 4096
vdso_blob_start:

; -------------------------------------------------------------
; Offset 0x000: gettimeofday(struct timeval *tv, struct timezone *tz)
; -------------------------------------------------------------
vdso_gettimeofday:
    test rdi, rdi
    jz .gtod_done

    rdtsc
    shl rdx, 32
    or rax, rdx                 ; rax = current tsc

    mov r8, [rel vdso_boot_tsc]
    sub rax, r8                 ; elapsed = current_tsc - boot_tsc

    mov r9, [rel vdso_tsc_hz]
    test r9, r9
    jz .gtod_fallback

    xor edx, edx
    div r9                      ; rax = sec, rdx = rem_cycles

    mov r8, rax                 ; r8 = sec
    mov rax, rdx                ; rax = rem_cycles
    mov rcx, 1000000            ; 10^6 (usec)
    mul rcx                     ; rdx:rax = rem_cycles * 10^6
    div r9                      ; rax = usec

    add r8, [rel vdso_boot_sec] ; sec += boot_sec
    mov [rdi], r8               ; tv->tv_sec = sec
    mov [rdi + 8], rax          ; tv->tv_usec = usec

.gtod_done:
    xor eax, eax
    ret

.gtod_fallback:
    mov eax, 96                 ; SYS_gettimeofday
    syscall
    ret

; Pad to offset 0x400 (1024)
times (1024 - ($ - vdso_blob_start)) db 0xCC

; -------------------------------------------------------------
; Offset 0x400: time(time_t *t)
; -------------------------------------------------------------
vdso_time:
    rdtsc
    shl rdx, 32
    or rax, rdx

    mov r8, [rel vdso_boot_tsc]
    sub rax, r8

    mov r9, [rel vdso_tsc_hz]
    test r9, r9
    jz .time_fallback

    xor edx, edx
    div r9                      ; rax = sec
    add rax, [rel vdso_boot_sec]; rax = now_sec

    test rdi, rdi
    jz .time_done
    mov [rdi], rax

.time_done:
    ret

.time_fallback:
    mov eax, 201                ; SYS_time
    syscall
    ret

; Pad to offset 0x800 (2048)
times (2048 - ($ - vdso_blob_start)) db 0xCC

; -------------------------------------------------------------
; Offset 0x800: getcpu(unsigned *cpu, unsigned *node, void *unused)
; -------------------------------------------------------------
vdso_getcpu:
    xor eax, eax
    test rdi, rdi
    jz .skip_cpu
    mov dword [rdi], 0
.skip_cpu:
    test rsi, rsi
    jz .skip_node
    mov dword [rsi], 0
.skip_node:
    ret

; Pad to offset 0xC00 (3072)
times (3072 - ($ - vdso_blob_start)) db 0xCC

; -------------------------------------------------------------
; Offset 0xC00: clock_gettime(clockid_t clk_id, struct timespec *tp)
; -------------------------------------------------------------
vdso_clock_gettime:
    test rsi, rsi
    jz .cgt_err_fault

    rdtsc
    shl rdx, 32
    or rax, rdx

    mov r8, [rel vdso_boot_tsc]
    sub rax, r8

    mov r9, [rel vdso_tsc_hz]
    test r9, r9
    jz .cgt_fallback

    xor edx, edx
    div r9                      ; rax = sec, rdx = rem_cycles

    mov r8, rax                 ; r8 = sec
    mov rax, rdx                ; rax = rem_cycles
    mov rcx, 1000000000         ; 10^9 (nsec)
    mul rcx
    div r9                      ; rax = nsec
    mov r10, rax                ; r10 = nsec

    ; Check if clock is REALTIME (0 or 4)
    test rdi, rdi
    jz .cgt_add_boot_sec
    cmp rdi, 4
    je .cgt_add_boot_sec
    jmp .cgt_store

.cgt_add_boot_sec:
    add r8, [rel vdso_boot_sec]

.cgt_store:
    mov [rsi], r8               ; tp->tv_sec
    mov [rsi + 8], r10          ; tp->tv_nsec
    xor eax, eax                ; return 0
    ret

.cgt_err_fault:
    mov rax, -14                ; -EFAULT
    ret

.cgt_fallback:
    mov eax, 228                ; SYS_clock_gettime
    syscall
    ret

; Pad to offset 0xE00 (3584)
times (3584 - ($ - vdso_blob_start)) db 0xCC

; -------------------------------------------------------------
; Offset 0xE00: Shared Time Data
; -------------------------------------------------------------
vdso_boot_tsc: dq 0
vdso_tsc_khz:  dq 0
vdso_boot_sec: dq 0
vdso_tsc_hz:   dq 0

; Pad to full 4096 bytes
times (4096 - ($ - vdso_blob_start)) db 0xCC

vdso_blob_end:
