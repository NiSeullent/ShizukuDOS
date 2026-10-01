; Opt-in protected-mode AP worker. SIPI 07h starts this 512-byte trampoline
; at physical 0000:7000 in real mode. Every AP installs the same immutable
; flat GDT, then uses a separate 512-byte stack indexed by its xAPIC ID.
; There are no DOS/Win98 calls and paging remains disabled.
bits 16
org 0x7000

CODE_SEL equ 0x08
DATA_SEL equ 0x10
ACK_COUNT equ 0x6000
JOB_READY equ 0x6004
DONE_COUNT equ 0x6008
FAIL_COUNT equ 0x600c
START_COUNT equ 0x6010
BSP_RUNNING equ 0x6014
OVERLAP_COUNT equ 0x6018
PM_COUNT equ 0x601c
BSP_PROGRESS equ 0x6020
ACK_IDS equ 0x6100
JOB_SLOTS equ 0x6200
AP_PROGRESS equ 0x6e00
AP_ITERATIONS equ 2000000
SPIN_LIMIT equ 2000000000
STACK_BOTTOM equ 0x30000
STACK_STRIDE equ 512

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE_SEL:pm_start

align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
gdt_end:
gdt_descriptor:
    dw gdt_end-gdt-1
    dd gdt

bits 32
pm_start:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, 1
    cpuid
    shr ebx, 24             ; initial xAPIC ID, 0..255
    ; Each possible ID owns [0x30000+ID*512, 0x30000+(ID+1)*512).
    ; Highest stack top is 0x50000, below conventional 640-KiB RAM.
    lea esp, [ebx+1]
    shl esp, 9
    add esp, STACK_BOTTOM
    push dword 0x534d5041  ; write/read the per-CPU stack before publication
    pop eax
    cmp eax, 0x534d5041
    jne .fail_no_slot
    mov esi, ACK_IDS
    add esi, ebx
    cmp byte [esi], 0
    jne .park               ; second SIPI must not count or redo work
    mov byte [esi], 1
%ifndef OMIT_PM_ENTRY
    lock inc dword [PM_COUNT]
%endif
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
    lea esi, [ebx+ebx*2]
    shl esi, 2
    add esi, JOB_SLOTS
    cmp dword [esi], 1
    jne .fail
    mov dword [esi], 2
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
    mov eax, [esi+4]
    xor edx, edx
    xor ecx, ecx
.work_loop:
    rol eax, 5
    xor eax, 0x9e3779b9
    add eax, edx
    inc edx
    test edx, 0x7fff
    jnz .after_progress
%ifndef OMIT_AP_PROGRESS
    inc byte [AP_PROGRESS+ebx] ; <=61 checkpoints, no byte wrap
%endif
.after_progress:
    cmp edx, AP_ITERATIONS/4
    jne .after_first_sample
    mov ecx, [BSP_PROGRESS]
.after_first_sample:
    cmp edx, AP_ITERATIONS*3/4
    jne .after_overlap
    cmp dword [BSP_RUNNING], 1
    jne .after_overlap
    cmp dword [BSP_PROGRESS], ecx
    jbe .after_overlap
    lock inc dword [OVERLAP_COUNT]
.after_overlap:
    cmp edx, AP_ITERATIONS
    jb .work_loop
    mov [esi+8], eax
    mov dword [esi], 3
%ifndef OMIT_DONE
    lock inc dword [DONE_COUNT]
%endif
    jmp .park

.fail:
    mov dword [esi], 4
.fail_no_slot:
    lock inc dword [FAIL_COUNT]
.park:
    hlt
    jmp .park

times 512-($-$$) db 0
