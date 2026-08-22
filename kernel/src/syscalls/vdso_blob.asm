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

    sub rax, [rel vdso_boot_tsc] ; rax = delta_cycles

    mov rcx, [rel vdso_tsc_sec_mult]
    test rcx, rcx
    jz .gtod_fallback

    mov r8, rax                 ; r8 = delta_cycles

    ; 1. sec = ((delta_cycles * M) >> 64) >> shift
    mul rcx                     ; rdx = high 64 bits of delta_cycles * M
    mov ecx, [rel vdso_tsc_sec_shift]
    shr rdx, cl
    mov r9, rdx                 ; r9 = sec

    ; 2. rem_cycles = delta_cycles - (sec * tsc_hz)
    mov rax, r9
    imul rax, [rel vdso_tsc_hz]
    sub r8, rax                 ; r8 = rem_cycles

    ; 3. usec = (rem_cycles * mult_rem_us) >> 32
    mov rax, r8
    mov rcx, [rel vdso_mult_rem_us]
    mul rcx
    shrd rax, rdx, 32           ; rax = usec (0 .. 999,999)

    add r9, [rel vdso_boot_sec]
    mov [rdi], r9               ; tv->tv_sec = sec
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

    sub rax, [rel vdso_boot_tsc]

    mov rcx, [rel vdso_tsc_sec_mult]
    test rcx, rcx
    jz .time_fallback

    mul rcx
    mov ecx, [rel vdso_tsc_sec_shift]
    shr rdx, cl
    add rdx, [rel vdso_boot_sec]

    test rdi, rdi
    jz .time_done
    mov [rdi], rdx

.time_done:
    mov rax, rdx
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

    sub rax, [rel vdso_boot_tsc] ; rax = delta_cycles

    mov rcx, [rel vdso_tsc_sec_mult]
    test rcx, rcx
    jz .cgt_fallback

    mov r8, rax                 ; r8 = delta_cycles

    ; 1. sec = ((delta_cycles * M) >> 64) >> shift
    mul rcx                     ; rdx = high 64 bits of delta_cycles * M
    mov ecx, [rel vdso_tsc_sec_shift]
    shr rdx, cl
    mov r9, rdx                 ; r9 = sec

    ; 2. rem_cycles = delta_cycles - (sec * tsc_hz)
    mov rax, r9
    imul rax, [rel vdso_tsc_hz]
    sub r8, rax                 ; r8 = rem_cycles

    ; 3. nsec = (rem_cycles * mult_rem_ns) >> 32
    mov rax, r8
    mov rcx, [rel vdso_mult_rem_ns]
    mul rcx
    shrd rax, rdx, 32           ; rax = nsec (0 .. 999,999,999)
    mov r10, rax                ; r10 = nsec

    ; Check if clock is REALTIME (0 or 4)
    test rdi, rdi
    jz .cgt_add_boot_sec
    cmp rdi, 4
    je .cgt_add_boot_sec
    jmp .cgt_store

.cgt_add_boot_sec:
    add r9, [rel vdso_boot_sec]

.cgt_store:
    mov [rsi], r9               ; tp->tv_sec
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
vdso_boot_tsc:       dq 0
vdso_tsc_khz:        dq 0
vdso_boot_sec:       dq 0
vdso_tsc_hz:         dq 0
vdso_tsc_sec_mult:   dq 0
vdso_tsc_sec_shift:  dq 0
vdso_mult_rem_ns:    dq 0
vdso_mult_rem_us:    dq 0


; Pad to full 4096 bytes
times (4096 - ($ - vdso_blob_start)) db 0xCC

vdso_blob_end:
