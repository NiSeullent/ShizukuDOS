; Isolated x86 BSP/AP work experiment for ShizukuDOS.
; BIOS floppy -> flat protected mode -> ACPI MADT AP IDs -> xAPIC INIT-SIPI
; -> assigned per-AP jobs with atomic barriers and independently checked work.
; No DOS INT API or Windows 98 VMM scheduling is modified here.
bits 16
org 0x8000

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
BSP_SEEN_COUNT equ 0x6024
ACK_IDS equ 0x6100
JOB_SLOTS equ 0x6200
AP_PROGRESS equ 0x6e00
BSP_TRACK equ 0x6f00
SIPI_VECTOR equ 0x07
MAX_APS equ 32
AP_ITERATIONS equ 2000000
BSP_ITERATIONS equ 16000000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xb000
    ; Fast A20 gate so the ACPI tables and xAPIC MMIO do not wrap at 1 MiB.
    in al, 0x92
    or al, 2
    and al, 0xfe
    out 0x92, al
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
    mov esp, 0xb000
    cld
    call serial_init
    mov esi, msg_banner
    call puts
    ; Clear shared counts and all 256 APIC-ID indexed job slots before any
    ; early failure report or INIT/SIPI can observe them.
    mov edi, ACK_COUNT
    mov ecx, 0x1000/4
    xor eax, eax
    rep stosd

%ifdef PROTECTED_AP
    ; The APIC-ID-indexed stacks end at 0x50000. Require at least 512 KiB
    ; conventional RAM per the BIOS data area before starting any AP.
    cmp word [0x413], 512
    jb error_memory
%endif

    call detect_lapic
    jc error_apic
    call find_madt
    jc error_acpi
    call collect_targets
    jc error_madt

    call prepare_jobs

    xor ebx, ebx
.start_next:
    cmp ebx, [target_count]
    jae .wait_acks
    mov al, [target_ids+ebx]
    call start_ap
    jc error_ipi
    inc ebx
    jmp .start_next

.wait_acks:
    mov esi, ACK_COUNT
    call wait_counter
    jc error_ack
    mov dword [JOB_READY], 1
    mov esi, START_COUNT
    call wait_counter
    jc error_start

    movzx eax, byte [bsp_id]
    xor eax, 0xb5b50000
    mov [bsp_seed], eax
    mov dword [BSP_RUNNING], 1
%ifdef STALL_BSP_AFTER_FLAG
    ; Fault injection: reproduce a BSP preemption after setting the legacy
    ; running flag. APs finish first, so progress overlap must stay zero.
    mov esi, DONE_COUNT
    call wait_counter
    jc error_done
%endif
    mov ecx, BSP_ITERATIONS
    call worker_hash_parallel
    mov [bsp_result], eax
    mov dword [BSP_RUNNING], 0

    mov esi, DONE_COUNT
    call wait_counter
    jc error_done
%ifdef PROTECTED_AP
    mov eax, [PM_COUNT]
    cmp eax, [target_count]
    jne error_pm
%endif
    cmp dword [FAIL_COUNT], 0
    jne error_worker
    mov eax, [OVERLAP_COUNT]
    cmp eax, [target_count]
    jne error_overlap
    mov eax, [BSP_SEEN_COUNT]
    cmp eax, [target_count]
    jne error_overlap
%ifdef FAULT_INJECT
    ; Deliberately corrupt one completed AP result to exercise the guest
    ; validation failure, while leaving the AP work and barriers intact.
    cmp dword [target_count], 0
    je .no_fault_target
    movzx edi, byte [target_ids]
    lea edi, [edi+edi*2]
    xor dword [JOB_SLOTS+edi*4+8], 1
.no_fault_target:
%endif
    call verify_jobs
    jc error_result
    call print_jobs
    call print_summary
    mov esi, msg_pass
    call puts
    call print_progress
%ifdef PROTECTED_AP
    call print_pm
%endif
    mov eax, 0x10
    jmp qemu_exit

prepare_jobs:
    xor ebx, ebx
.next:
    cmp ebx, [target_count]
    jae .done
    movzx edx, byte [target_ids+ebx]
    lea edi, [edx+edx*2]
    shl edi, 2
    add edi, JOB_SLOTS
    mov dword [edi], 1
    mov eax, 0x5a17c0de
    xor eax, edx
    mov [edi+4], eax
    mov dword [edi+8], 0
    inc ebx
    jmp .next
.done:
    ret

worker_hash:
    ; EAX seed, ECX iterations. EAX returns a deterministic 32-bit result.
    xor edx, edx
.loop:
    rol eax, 5
    xor eax, 0x9e3779b9
    add eax, edx
    inc edx
    cmp edx, ecx
    jb .loop
    ret

worker_hash_parallel:
    ; Publish progress only after real BSP hash iterations. Verification uses
    ; worker_hash above and never alters this concurrent-work evidence.
    xor edx, edx
.loop:
    rol eax, 5
    xor eax, 0x9e3779b9
    add eax, edx
    inc edx
    test edx, 0x3fff       ; checkpoint every 16,384 real iterations
    jnz .next
%ifndef OMIT_BSP_PROGRESS
    mov [BSP_PROGRESS], edx
%endif
    pushad
    call observe_ap_progress
    popad
.next:
    cmp edx, ecx
    jb .loop
    ret

observe_ap_progress:
    ; Two observations during distinct BSP work checkpoints are required.
    ; Progress is a monotonic 0..61 byte per AP. BSP_TRACK high bits encode
    ; baseline-taken (0x40) and progress-observed (0x80).
    xor ebx, ebx
.next:
    cmp ebx, [target_count]
    jae .done
    movzx edi, byte [target_ids+ebx]
    movzx eax, byte [AP_PROGRESS+edi]
    mov dl, [BSP_TRACK+edi]
    test dl, 0x80
    jnz .advance
    test dl, 0x40
    jz .baseline
    and dl, 0x3f
    cmp al, dl
    jbe .advance
    or al, 0xc0
    mov [BSP_TRACK+edi], al
    inc dword [BSP_SEEN_COUNT]
    jmp .advance
.baseline:
    or al, 0x40
    mov [BSP_TRACK+edi], al
.advance:
    inc ebx
    jmp .next
.done:
    ret

verify_jobs:
    xor ebx, ebx
.next:
    cmp ebx, [target_count]
    jae .good
    movzx edi, byte [target_ids+ebx]
    lea edi, [edi+edi*2]
    shl edi, 2
    add edi, JOB_SLOTS
    cmp dword [edi], 3
    jne .bad
    mov eax, [edi+4]
    mov ecx, AP_ITERATIONS
    call worker_hash
    cmp eax, [edi+8]
    jne .bad
    inc ebx
    jmp .next
.good:
    clc
    ret
.bad:
    stc
    ret

print_jobs:
    xor ebx, ebx
.next:
    cmp ebx, [target_count]
    jae .bsp
    movzx edi, byte [target_ids+ebx]
    lea edi, [edi+edi*2]
    shl edi, 2
    add edi, JOB_SLOTS
    mov esi, msg_work_ap
    call puts
    mov al, [target_ids+ebx]
    call print_hex8
    mov esi, msg_work_seed
    call puts
    mov eax, [edi+4]
    call print_hex32
    mov esi, msg_work_result
    call puts
    mov eax, [edi+8]
    call print_hex32
    mov esi, msg_newline
    call puts
    inc ebx
    jmp .next
.bsp:
    mov esi, msg_work_bsp
    call puts
    mov al, [bsp_id]
    call print_hex8
    mov esi, msg_work_seed
    call puts
    mov eax, [bsp_seed]
    call print_hex32
    mov esi, msg_work_result
    call puts
    mov eax, [bsp_result]
    call print_hex32
    mov esi, msg_newline
    call puts
    ret

print_summary:
    mov esi, msg_result
    call puts
    mov al, [bsp_id]
    call print_hex8
    mov esi, msg_expected
    call puts
    mov al, [target_count]
    call print_hex8
    mov esi, msg_ack
    call puts
    mov al, [ACK_COUNT]
    call print_hex8
    mov esi, msg_started
    call puts
    mov al, [START_COUNT]
    call print_hex8
    mov esi, msg_done
    call puts
    mov al, [DONE_COUNT]
    call print_hex8
    mov esi, msg_overlap
    call puts
    mov al, [OVERLAP_COUNT]
    call print_hex8
    mov esi, msg_failures
    call puts
    mov al, [FAIL_COUNT]
    call print_hex8
    ret

print_progress:
    mov esi, msg_progress
    call puts
    mov al, [BSP_SEEN_COUNT]
    call print_hex8
    mov esi, msg_ap_seen
    call puts
    mov al, [OVERLAP_COUNT]
    call print_hex8
    mov esi, msg_newline
    call puts
    ret

error_apic:
    mov esi, msg_error_apic
    jmp error_exit
error_acpi:
    mov esi, msg_error_acpi
    jmp error_exit
error_madt:
    mov esi, msg_error_madt
    jmp error_exit
error_ipi:
    mov esi, msg_error_ipi
    jmp error_exit
error_ack:
    mov esi, msg_error_ack
    jmp error_exit
error_start:
    mov esi, msg_error_start
    jmp error_exit
error_done:
    mov esi, msg_error_done
    jmp error_exit
error_worker:
    mov esi, msg_error_worker
    jmp error_exit
error_overlap:
    mov esi, msg_error_overlap
    jmp error_exit
error_result:
    mov esi, msg_error_result
%ifdef PROTECTED_AP
    jmp error_exit
error_memory:
    mov esi, msg_error_memory
    jmp error_exit
error_pm:
    mov esi, msg_error_pm
%endif
error_exit:
    call puts
    call print_summary
    mov esi, msg_fail
    call puts
    call print_progress
%ifdef PROTECTED_AP
    call print_pm
%endif
    mov eax, 0x11
qemu_exit:
    mov dx, 0xf4
    out dx, eax                ; isa-debug-exit, exit status (value << 1)|1
.halt:
    cli
    hlt
    jmp .halt

detect_lapic:
    mov eax, 1
    cpuid
    test edx, 1 << 9          ; CPUID local APIC feature
    jz .bad
    mov ecx, 0x1b             ; IA32_APIC_BASE MSR
    rdmsr
    test eax, 1 << 11         ; global APIC enable
    jz .bad
    test edx, edx             ; this 32-bit prototype requires APIC < 4 GiB
    jnz .bad
    and eax, 0xfffff000
    mov [lapic_base], eax
    mov edi, eax
    mov eax, [edi+0x20]       ; local APIC ID register
    shr eax, 24
    mov [bsp_id], al
    mov eax, [edi+0xf0]       ; spurious-interrupt vector register
    and eax, 0xffffff00
    or eax, 0x1ff            ; vector FFh + software APIC enable
    mov [edi+0xf0], eax
    clc
    ret
.bad:
    stc
    ret

find_madt:
    ; ACPI 1.0 RSDP: EBDA first KiB, then BIOS 0xe0000..0xfffff.
    movzx esi, word [0x40e]
    shl esi, 4
    mov ecx, 64
    call scan_rsdp
    jnc .rsdp_found
    mov esi, 0xe0000
    mov ecx, 0x20000/16
    call scan_rsdp
    jc .bad
.rsdp_found:
    mov ebp, [esi+16]         ; RSDT physical address
    mov esi, ebp
    cmp dword [esi], 0x54445352  ; "RSDT"
    jne .bad
    call check_sdt
    jc .bad
    mov edx, [ebp+4]
    sub edx, 36
    test edx, 3
    jnz .bad
    shr edx, 2
    lea ebx, [ebp+36]
.next_table:
    test edx, edx
    jz .bad
    mov edi, [ebx]
    cmp dword [edi], 0x43495041  ; "APIC" MADT signature
    jne .advance
    mov esi, edi
    call check_sdt
    jc .bad
    cmp dword [edi+4], 44
    jb .bad
    mov [madt_base], edi
    clc
    ret
.advance:
    add ebx, 4
    dec edx
    jmp .next_table
.bad:
    stc
    ret

scan_rsdp:
    ; Inputs ESI address, ECX 16-byte slots. Returns ESI at valid RSDP.
.next:
    cmp dword [esi], 0x20445352    ; "RSD "
    jne .advance
    cmp dword [esi+4], 0x20525450  ; "PTR "
    jne .advance
    push ecx
    push esi
    mov edi, esi
    mov ecx, 20
    xor eax, eax
.checksum:
    add al, [edi]
    inc edi
    dec ecx
    jnz .checksum
    pop esi
    pop ecx
    test al, al
    jz .found
.advance:
    add esi, 16
    dec ecx
    jnz .next
    stc
    ret
.found:
    clc
    ret

check_sdt:
    ; SDT checksum and bounded length; preserve all general registers.
    pushad
    mov ecx, [esi+4]
    cmp ecx, 36
    jb .bad
    cmp ecx, 65536
    ja .bad
    xor eax, eax
    mov ebx, esi
.sum:
    add al, [ebx]
    inc ebx
    dec ecx
    jnz .sum
    test al, al
    jnz .bad
    popad
    clc
    ret
.bad:
    popad
    stc
    ret

collect_targets:
    mov esi, [madt_base]
    mov edi, [esi+4]
    add edi, esi              ; exclusive end of MADT
    add esi, 44               ; skip 36-byte SDT header + 8-byte MADT body
    xor ebx, ebx
    mov byte [bsp_record_seen], 0
.next:
    cmp esi, edi
    jae .done
    movzx ecx, byte [esi+1]
    cmp ecx, 2
    jb .bad
    lea eax, [esi+ecx]
    cmp eax, edi
    ja .bad
    cmp byte [esi], 0         ; MADT processor local APIC entry
    jne .other
    cmp ecx, 8
    jb .bad
    test dword [esi+4], 1     ; enabled processor only
    jz .advance
    mov al, [esi+3]           ; xAPIC destination ID
    cmp al, [bsp_id]
    jne .check_duplicate
    cmp byte [bsp_record_seen], 0
    jne .bad
    mov byte [bsp_record_seen], 1
    jmp .advance
.check_duplicate:
%ifdef DUPLICATE_MADT
    ; Synthesize a repeated enabled target before IPI to exercise rejection.
    cmp ebx, 0
    jne .normal_duplicate_check
    mov [target_ids], al
    mov ebx, 1
.normal_duplicate_check:
%endif
    xor edx, edx
.duplicate_scan:
    cmp edx, ebx
    jae .unique
    cmp al, [target_ids+edx]
    je .bad
    inc edx
    jmp .duplicate_scan
.unique:
    cmp ebx, MAX_APS
    jae .bad
    mov [target_ids+ebx], al
    inc ebx
    jmp .advance
.other:
    cmp byte [esi], 9         ; enabled x2APIC entries require new ICR path
    jne .advance
    cmp ecx, 16
    jb .bad
    test dword [esi+8], 1
    jnz .bad
.advance:
    add esi, ecx
    jmp .next
.done:
    cmp byte [bsp_record_seen], 1
    jne .bad
    mov [target_count], ebx
    clc
    ret
.bad:
    stc
    ret

start_ap:
    ; AL is the MADT APIC ID. Send INIT assert/deassert then two SIPIs.
    mov [current_target], al
    mov edi, [lapic_base]
    movzx eax, byte [current_target]
    shl eax, 24
    mov [edi+0x310], eax
    mov dword [edi+0x300], 0x0000c500 ; INIT, level assert, level trigger
    call wait_icr
    jc .bad
    mov ax, 11932            ; PIT channel 2: about 10 ms
    call pit_delay
    jc .bad
    mov dword [edi+0x300], 0x00008500 ; INIT deassert
    call wait_icr
    jc .bad
    mov ax, 239              ; about 200 us
    call pit_delay
    jc .bad
    mov dword [edi+0x300], 0x00000600 | SIPI_VECTOR
    call wait_icr
    jc .bad
    mov ax, 239
    call pit_delay
    jc .bad
    mov dword [edi+0x300], 0x00000600 | SIPI_VECTOR
    call wait_icr
    ret
.bad:
    stc
    ret

wait_icr:
    mov ecx, 10000000
.poll:
    test dword [edi+0x300], 1 << 12 ; delivery status
    jz .ready
    dec ecx
    jnz .poll
    stc
    ret
.ready:
    clc
    ret

pit_delay:
    ; AX PIT input ticks, 1.193182 MHz. Returns CF on bounded poll timeout.
    push eax
    push ebx
    push ecx
    push edx
    mov bx, ax
    mov dx, 0x61
    in al, dx
    and al, 0xfc            ; gate low, speaker off
    out dx, al
    mov dx, 0x43
    mov al, 0xb0            ; channel 2, lobyte/hibyte, mode 0 one-shot
    out dx, al
    mov dx, 0x42
    mov ax, bx
    out dx, al
    mov al, ah
    out dx, al
    mov dx, 0x61
    in al, dx
    or al, 1               ; start channel 2 count
    out dx, al
    mov ecx, 20000000
.poll:
    in al, dx
    test al, 0x20          ; PIT channel 2 output
    jnz .done
    dec ecx
    jnz .poll
    pop edx
    pop ecx
    pop ebx
    pop eax
    stc
    ret
.done:
    pop edx
    pop ecx
    pop ebx
    pop eax
    clc
    ret

wait_counter:
    ; ESI points to ACK_COUNT, START_COUNT or DONE_COUNT. A 2-second PIT
    ; budget applies independently to each phase, with AP error early exit.
    mov ebx, 40             ; <= 2 seconds in 50 ms PIT slices
.poll:
    mov eax, [esi]
    cmp eax, [target_count]
    je .done
    cmp dword [FAIL_COUNT], 0
    jne .bad
    mov ax, 59659
    call pit_delay
    jc .bad
    dec ebx
    jnz .poll
.bad:
    stc
    ret
.done:
    clc
    ret

serial_init:
    mov dx, 0x3fb
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 1               ; 115200 baud divisor
    out dx, al
    mov dx, 0x3f9
    xor al, al
    out dx, al
    mov dx, 0x3fb
    mov al, 3               ; 8N1
    out dx, al
    mov dx, 0x3fa
    mov al, 0xc7
    out dx, al
    mov dx, 0x3fc
    mov al, 3
    out dx, al
    ret

puts:
    lodsb
    test al, al
    jz .done
    call putc
    jmp puts
.done:
    ret

putc:
    push eax
    push ecx
    push edx
    mov dx, 0x3fd
    mov ecx, 1000000
.ready:
    in al, dx
    test al, 0x20
    jnz .send
    dec ecx
    jnz .ready
    jmp .done
.send:
    pop edx
    pop ecx
    pop eax
    push edx
    mov dx, 0x3f8
    out dx, al
    pop edx
    ret
.done:
    pop edx
    pop ecx
    pop eax
    ret

print_hex8:
    push eax
    mov ah, al
    shr al, 4
    call print_nibble
    mov al, ah
    and al, 0x0f
    call print_nibble
    pop eax
    ret
print_hex32:
    pushad
    mov ebp, eax
    mov ecx, 8
.next:
    mov eax, ebp
    shr eax, 28
    call print_nibble
    shl ebp, 4
    dec ecx
    jnz .next
    popad
    ret
print_nibble:
    cmp al, 10
    jb .digit
    add al, 7
.digit:
    add al, '0'
    call putc
    ret

%ifdef PROTECTED_AP
print_pm:
    mov esi, msg_pm_entered
    call puts
    mov al, [PM_COUNT]
    call print_hex8
    mov esi, msg_pm_stack
    call puts
    ret
%endif

%ifdef PROTECTED_AP
msg_banner db 'SMP STAGE2: xAPIC protected-mode AP work prototype', 13, 10, 0
%else
msg_banner db 'SMP STAGE2: xAPIC parallel work prototype', 13, 10, 0
%endif
msg_work_ap db 'SMP WORK ap=0x', 0
msg_work_bsp db 'SMP WORK bsp=0x', 0
msg_work_seed db ' seed=0x', 0
msg_work_result db ' result=0x', 0
msg_newline db 13, 10, 0
msg_result db 'SMP RESULT bsp=0x', 0
msg_expected db ' expected=0x', 0
msg_ack db ' ack=0x', 0
msg_started db ' started=0x', 0
msg_done db ' done=0x', 0
msg_overlap db ' overlap=0x', 0
msg_failures db ' failures=0x', 0
msg_progress db 'SMP PROGRESS bsp_seen=0x', 0
msg_ap_seen db ' ap_seen=0x', 0
msg_pass db ' PASS', 13, 10, 0
msg_fail db ' FAIL', 13, 10, 0
msg_error_apic db 'SMP ERROR: local APIC unavailable', 13, 10, 0
msg_error_acpi db 'SMP ERROR: valid ACPI MADT not found', 13, 10, 0
msg_error_madt db 'SMP ERROR: unsupported or malformed MADT processors', 13, 10, 0
msg_error_ipi db 'SMP ERROR: INIT/SIPI delivery or PIT timeout', 13, 10, 0
msg_error_ack db 'SMP ERROR: AP acknowledgement timeout', 13, 10, 0
msg_error_start db 'SMP ERROR: AP work-start timeout or AP failure', 13, 10, 0
msg_error_done db 'SMP ERROR: AP work completion timeout or AP failure', 13, 10, 0
msg_error_worker db 'SMP ERROR: AP reported work failure', 13, 10, 0
msg_error_overlap db 'SMP ERROR: APs did not overlap BSP work phase', 13, 10, 0
msg_error_result db 'SMP ERROR: AP result mismatch', 13, 10, 0
%ifdef PROTECTED_AP
msg_error_memory db 'SMP ERROR: insufficient conventional RAM for AP stacks', 13, 10, 0
msg_error_pm db 'SMP ERROR: AP protected-mode entry count mismatch', 13, 10, 0
msg_pm_entered db 'SMP PM entered=0x', 0
msg_pm_stack db ' stack_bytes=512 range=0x30000..0x50000', 13, 10, 0
%endif

align 4
lapic_base dd 0
madt_base dd 0
target_count dd 0
bsp_seed dd 0
bsp_result dd 0
bsp_id db 0
bsp_record_seen db 0
current_target db 0
target_ids times MAX_APS db 0

times 8192-($-$$) db 0
