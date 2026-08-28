section .rodata
global vdso_blob_start
global vdso_blob_end

align 4096
vdso_blob_start:

; ==============================================================================
; ELF Header (64 bytes)
; ==============================================================================
elf_header:
    db 0x7F, "ELF"                      ; e_ident[EI_MAG0..3]
    db 2                                ; EI_CLASS = ELFCLASS64 (2)
    db 1                                ; EI_DATA = ELFDATA2LSB (1)
    db 1                                ; EI_VERSION = EV_CURRENT (1)
    db 0                                ; EI_OSABI = ELFOSABI_NONE / SYSV (0)
    db 0                                ; EI_ABIVERSION = 0
    times 7 db 0                        ; EI_PAD
    dw 3                                ; e_type = ET_DYN (3)
    dw 62                               ; e_machine = EM_X86_64 (62)
    dd 1                                ; e_version = EV_CURRENT (1)
    dq 0                                ; e_entry = 0
    dq 64                               ; e_phoff = offset of program headers (64)
    dq 0                                ; e_shoff = no section headers (0)
    dd 0                                ; e_flags = 0
    dw 64                               ; e_ehsize = 64
    dw 56                               ; e_phentsize = 56
    dw 2                                ; e_phnum = 2 (PT_LOAD, PT_DYNAMIC)
    dw 0                                ; e_shentsize = 0
    dw 0                                ; e_shnum = 0
    dw 0                                ; e_shstrndx = 0

; ==============================================================================
; Program Headers (56 bytes * 2 = 112 bytes) (offset 0x040)
; ==============================================================================
program_headers:
    ; 1. PT_LOAD segment covering the whole 4KB page
    dd 1                                ; p_type = PT_LOAD (1)
    dd 5                                ; p_flags = PF_R | PF_X (5)
    dq 0                                ; p_offset = 0
    dq 0                                ; p_vaddr = 0
    dq 0                                ; p_paddr = 0
    dq 4096                             ; p_filesz = 4096
    dq 4096                             ; p_memsz = 4096
    dq 4096                             ; p_align = 4096

    ; 2. PT_DYNAMIC segment pointing to .dynamic
    dd 2                                ; p_type = PT_DYNAMIC (2)
    dd 4                                ; p_flags = PF_R (4)
    dq (vdso_dynamic - vdso_blob_start) ; p_offset
    dq (vdso_dynamic - vdso_blob_start) ; p_vaddr
    dq (vdso_dynamic - vdso_blob_start) ; p_paddr
    dq (vdso_dynamic_end - vdso_dynamic); p_filesz
    dq (vdso_dynamic_end - vdso_dynamic); p_memsz
    dq 8                                ; p_align = 8

; ==============================================================================
; Dynamic Section (.dynamic)
; ==============================================================================
align 8
vdso_dynamic:
    dq 4, (vdso_hash - vdso_blob_start)     ; DT_HASH (4)
    dq 5, (vdso_strtab - vdso_blob_start)   ; DT_STRTAB (5)
    dq 6, (vdso_symtab - vdso_blob_start)   ; DT_SYMTAB (6)
    dq 10, (vdso_strtab_end - vdso_strtab)  ; DT_STRSZ (10)
    dq 11, 24                               ; DT_SYMENT (11) = sizeof(Elf64_Sym)
    dq 0, 0                                 ; DT_NULL (0)
vdso_dynamic_end:

; ==============================================================================
; Hash Table (.hash)
; SysV ELF Hash Table: nbucket, nchain, bucket[nbucket], chain[nchain]
; ==============================================================================
align 8
vdso_hash:
    dd 1                                ; nbucket = 1
    dd 9                                ; nchain = 9 (symbols 0..8)
    dd 1                                ; bucket[0] = 1 (start with symbol 1)
    dd 0                                ; chain[0] = 0
    dd 2                                ; chain[1] -> 2
    dd 3                                ; chain[2] -> 3
    dd 4                                ; chain[3] -> 4
    dd 5                                ; chain[4] -> 5
    dd 6                                ; chain[5] -> 6
    dd 7                                ; chain[6] -> 7
    dd 8                                ; chain[7] -> 8
    dd 0                                ; chain[8] -> 0 (end of chain)

; ==============================================================================
; Dynamic Symbol Table (.dynsym)
; Elf64_Sym (24 bytes each)
; ==============================================================================
align 8
vdso_symtab:
    ; Symbol 0: STN_UNDEF
    dd 0
    db 0, 0
    dw 0
    dq 0, 0

    ; Symbol 1: __vdso_clock_gettime
    dd (str_vdso_clock_gettime - vdso_strtab)
    db 0x12, 0                          ; STB_GLOBAL | STT_FUNC
    dw 1                                ; st_shndx = 1
    dq (vdso_clock_gettime - vdso_blob_start)
    dq 128

    ; Symbol 2: clock_gettime
    dd (str_clock_gettime - vdso_strtab)
    db 0x12, 0
    dw 1
    dq (vdso_clock_gettime - vdso_blob_start)
    dq 128

    ; Symbol 3: __vdso_gettimeofday
    dd (str_vdso_gettimeofday - vdso_strtab)
    db 0x12, 0
    dw 1
    dq (vdso_gettimeofday - vdso_blob_start)
    dq 128

    ; Symbol 4: gettimeofday
    dd (str_gettimeofday - vdso_strtab)
    db 0x12, 0
    dw 1
    dq (vdso_gettimeofday - vdso_blob_start)
    dq 128

    ; Symbol 5: __vdso_time
    dd (str_vdso_time - vdso_strtab)
    db 0x12, 0
    dw 1
    dq (vdso_time - vdso_blob_start)
    dq 64

    ; Symbol 6: time
    dd (str_time - vdso_strtab)
    db 0x12, 0
    dw 1
    dq (vdso_time - vdso_blob_start)
    dq 64

    ; Symbol 7: __vdso_getcpu
    dd (str_vdso_getcpu - vdso_strtab)
    db 0x12, 0
    dw 1
    dq (vdso_getcpu - vdso_blob_start)
    dq 32

    ; Symbol 8: getcpu
    dd (str_getcpu - vdso_strtab)
    db 0x12, 0
    dw 1
    dq (vdso_getcpu - vdso_blob_start)
    dq 32
vdso_symtab_end:

; ==============================================================================
; Dynamic String Table (.dynstr)
; ==============================================================================
vdso_strtab:
    db 0
str_vdso_clock_gettime:
    db "__vdso_clock_gettime", 0
str_clock_gettime:
    db "clock_gettime", 0
str_vdso_gettimeofday:
    db "__vdso_gettimeofday", 0
str_gettimeofday:
    db "gettimeofday", 0
str_vdso_time:
    db "__vdso_time", 0
str_time:
    db "time", 0
str_vdso_getcpu:
    db "__vdso_getcpu", 0
str_getcpu:
    db "getcpu", 0
vdso_strtab_end:

; Pad to offset 0x400 (1024) for vsyscall time
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

; Pad to offset 0x600 for vsyscall gettimeofday
times (1536 - ($ - vdso_blob_start)) db 0xCC

; -------------------------------------------------------------
; Offset 0x600: gettimeofday(struct timeval *tv, struct timezone *tz)
; -------------------------------------------------------------
vdso_gettimeofday:
    test rdi, rdi
    jz .gtod_done

    rdtsc
    shl rdx, 32
    or rax, rdx

    sub rax, [rel vdso_boot_tsc]

    mov rcx, [rel vdso_tsc_sec_mult]
    test rcx, rcx
    jz .gtod_fallback

    mov r8, rax

    ; 1. sec = ((delta_cycles * M) >> 64) >> shift
    mul rcx
    mov ecx, [rel vdso_tsc_sec_shift]
    shr rdx, cl
    mov r9, rdx

    ; 2. rem_cycles = delta_cycles - (sec * tsc_hz)
    mov rax, r9
    imul rax, [rel vdso_tsc_hz]
    sub r8, rax

    ; 3. usec = (rem_cycles * mult_rem_us) >> 32
    mov rax, r8
    mov rcx, [rel vdso_mult_rem_us]
    mul rcx
    shrd rax, rdx, 32

    add r9, [rel vdso_boot_sec]
    mov [rdi], r9
    mov [rdi + 8], rax

.gtod_done:
    xor eax, eax
    ret

.gtod_fallback:
    mov eax, 96                 ; SYS_gettimeofday
    syscall
    ret

; Pad to offset 0x800 (2048) for vsyscall getcpu
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

; Pad to offset 0xC00 (3072) for clock_gettime
times (3072 - ($ - vdso_blob_start)) db 0xCC

; -------------------------------------------------------------
; Offset 0xC00: clock_gettime(clockid_t clk_id, struct timespec *tp)
; -------------------------------------------------------------
vdso_clock_gettime:
    test rsi, rsi
    jz .cgt_err_fault

    ; Check supported clocks:
    ; 0: CLOCK_REALTIME
    ; 1: CLOCK_MONOTONIC
    ; 4: CLOCK_MONOTONIC_RAW
    ; 5: CLOCK_REALTIME_COARSE
    ; 6: CLOCK_MONOTONIC_COARSE
    ; 7: CLOCK_BOOTTIME
    cmp rdi, 7
    ja .cgt_fallback
    cmp rdi, 2
    je .cgt_fallback
    cmp rdi, 3
    je .cgt_fallback

    rdtsc
    shl rdx, 32
    or rax, rdx

    sub rax, [rel vdso_boot_tsc] ; delta_cycles

    mov rcx, [rel vdso_tsc_sec_mult]
    test rcx, rcx
    jz .cgt_fallback

    mov r8, rax                 ; delta_cycles

    ; 1. sec = ((delta_cycles * M) >> 64) >> shift
    mul rcx
    mov ecx, [rel vdso_tsc_sec_shift]
    shr rdx, cl
    mov r9, rdx                 ; sec

    ; 2. rem_cycles = delta_cycles - (sec * tsc_hz)
    mov rax, r9
    imul rax, [rel vdso_tsc_hz]
    sub r8, rax

    ; 3. nsec = (rem_cycles * mult_rem_ns) >> 32
    mov rax, r8
    mov rcx, [rel vdso_mult_rem_ns]
    mul rcx
    shrd rax, rdx, 32           ; nsec (0 .. 999,999,999)
    mov r10, rax

    ; Check if clock is REALTIME (0 or 5)
    test rdi, rdi
    jz .cgt_add_boot_sec
    cmp rdi, 5
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

; Pad to offset 0xE00 (3584) for Shared Time Data
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
