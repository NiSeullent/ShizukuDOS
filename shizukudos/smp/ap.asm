; SIPI 07h enters physical 0000:7000 in 16-bit real mode. No DOS or Win98
; scheduler is entered. Each AP uses its CPUID initial APIC ID as an index
; into the shared job table and performs a bounded, deterministic hash job.
bits 16
org 0x7000

ACK_COUNT equ 0x6000
JOB_READY equ 0x6004
DONE_COUNT equ 0x6008
FAIL_COUNT equ 0x600c
START_COUNT equ 0x6010
BSP_RUNNING equ 0x6014
OVERLAP_COUNT equ 0x6018
BSP_PROGRESS equ 0x6020
ACK_IDS equ 0x6100
JOB_SLOTS equ 0x6200
AP_PROGRESS equ 0x6e00
AP_ITERATIONS equ 2000000
SPIN_LIMIT equ 2000000000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, 1
    cpuid
    shr ebx, 24             ; initial xAPIC ID, 0..255
    mov si, ACK_IDS
    add si, bx
    cmp byte [si], 0
    jne .park              ; ignore a duplicate SIPI
    mov byte [si], 1
    lock inc dword [ACK_COUNT]

    mov ecx, SPIN_LIMIT
.wait_job:
    cmp dword [JOB_READY], 1
    je .job_announced
    pause
    dec ecx
    jnz .wait_job
    jmp .fail_no_slot
.job_announced:
    mov ax, bx
    mov cx, 12
    mul cx                  ; AX = APIC ID * 12, max 3060
    mov si, JOB_SLOTS
    add si, ax
    cmp dword [si], 1       ; BSP assigned this AP a job
    jne .fail
    mov dword [si], 2
    lock inc dword [START_COUNT]

    mov ecx, SPIN_LIMIT
.wait_bsp:
    cmp dword [BSP_RUNNING], 1
    je .begin_work
    pause
    dec ecx
    jnz .wait_bsp
    jmp .fail
.begin_work:
    mov eax, [si+4]         ; unique input seed assigned by BSP
    xor edx, edx
    xor ecx, ecx             ; BSP progress sampled during AP work
.work_loop:
    rol eax, 5
    xor eax, 0x9e3779b9
    add eax, edx
    inc edx
    test edx, 0x7fff
    jnz .after_progress
%ifndef OMIT_AP_PROGRESS
    inc byte [AP_PROGRESS+bx] ; <=61 checkpoints, no byte wrap
%endif
.after_progress:
    cmp edx, AP_ITERATIONS/4
    jne .after_first_sample
    mov ecx, [BSP_PROGRESS]
.after_first_sample:
    cmp edx, AP_ITERATIONS*3/4
    jne .after_overlap
    ; The AP must execute one million iterations between BSP progress reads.
    ; A running flag alone cannot prove the BSP actually advanced.
    cmp dword [BSP_RUNNING], 1
    jne .after_overlap
    cmp dword [BSP_PROGRESS], ecx
    jbe .after_overlap
    lock inc dword [OVERLAP_COUNT]
.after_overlap:
    cmp edx, AP_ITERATIONS
    jb .work_loop
    mov [si+8], eax
    mov dword [si], 3       ; done
%ifndef OMIT_DONE
    lock inc dword [DONE_COUNT] ; release result before BSP observes count
%endif
    jmp .park

.fail:
    mov dword [si], 4       ; error status
.fail_no_slot:
    lock inc dword [FAIL_COUNT]
.park:
    hlt
    jmp .park

times 512-($-$$) db 0
