; Independent 32-bit PAE paging probe. It is not a Windows 98 VMM patch.
; ACPI E820 type-1 RAM at physical 0x1_0000_1000 is aliased at virtual
; 0xc000_1000 using the high dword of a 2 MiB PAE PDE. The probe writes,
; verifies, and restores one dword; physical 0x1000 is an alias guard.
bits 16
org 0x8000

CODE_SEL equ 0x08
DATA_SEL equ 0x10
E820_BUF equ 0x6000
PDPT equ 0x10000
PD_BASE equ 0x11000
HIGH_VA equ 0xc0001000
LOW_ALIAS equ 0x00001000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xb000
    mov byte [e820_ok], 0
    xor ebx, ebx
    mov bp, 128             ; finite BIOS E820 descriptor scan
.e820_next:
    mov dword [E820_BUF+20], 1
    mov eax, 0xe820
    mov edx, 0x534d4150   ; 'SMAP'
    mov ecx, 24
    mov di, E820_BUF
    int 0x15
    jc .e820_done
    cmp eax, 0x534d4150
    jne .e820_done
    cmp ecx, 20
    jb .e820_done
    cmp dword [E820_BUF+16], 1 ; usable RAM only
    jne .continue
    cmp ecx, 24
    jb .check_range
    test dword [E820_BUF+20], 1 ; extended attributes valid
    jz .continue
.check_range:
    ; Require one contiguous usable descriptor containing [4 GiB, 4 GiB+2 MiB).
    mov eax, [E820_BUF]
    mov edx, [E820_BUF+4]
    cmp edx, 1
    ja .continue
    jb .base_ok
    test eax, eax
    jnz .continue
.base_ok:
    add eax, [E820_BUF+8]
    adc edx, [E820_BUF+12]
    cmp edx, 1
    jb .continue
    ja .found
    cmp eax, 0x200000
    jb .continue
.found:
    mov [e820_end_lo], eax
    mov [e820_end_hi], edx
    mov byte [e820_ok], 1
    jmp .e820_done
.continue:
    test ebx, ebx
    jz .e820_done
    dec bp
    jnz .e820_next
.e820_done:
    ; BIOS calls are finished before entering protected mode.
    in al, 0x92
    or al, 2
    and al, 0xfe
    out 0x92, al            ; fast A20
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
    lidt [idt_descriptor]
    call serial_init
    mov esi, msg_banner
    call puts
    cmp byte [e820_ok], 1
    jne error_e820
    mov eax, 1
    cpuid
    test edx, 1 << 6       ; CPUID.01H:EDX.PAE
    jz error_cpu
    mov esi, msg_e820
    call puts
    mov eax, [e820_end_hi]
    call print_hex32
    mov eax, [e820_end_lo]
    call print_hex32
    mov esi, msg_newline
    call puts

    mov eax, cr0
    mov [old_cr0], eax
    mov eax, cr3
    mov [old_cr3], eax
    mov eax, cr4
    mov [old_cr4], eax
    call make_tables

    mov eax, cr4
    or eax, 1 << 5         ; PAE
    mov cr4, eax
    mov eax, PDPT
    mov cr3, eax
    mov eax, cr0
    or eax, 1 << 31        ; enable paging after PAE and CR3
    mov cr0, eax
    jmp .paged             ; serialize instruction fetch
.paged:
    mov eax, cr4
    test eax, 1 << 5
    jz .probe_fail
    mov eax, cr0
    test eax, 1 << 31
    jz .probe_fail
    cmp dword [PD_BASE+3*4096], 0x83
    jne .probe_fail
    cmp dword [PD_BASE+3*4096+4], 1
    jne .probe_fail
    mov esi, msg_paging
    call puts
    mov eax, [LOW_ALIAS]
    mov [low_guard], eax
    mov eax, [HIGH_VA]
    mov [high_original], eax
    xor eax, 0xa55a96c3
    mov [high_pattern], eax
    mov [HIGH_VA], eax
    mov eax, [HIGH_VA]
    mov [high_observed], eax
    cmp eax, [high_pattern]
    jne .restore_fail
    mov eax, [LOW_ALIAS]
    mov [low_guard_during], eax
    cmp eax, [low_guard]
    jne .restore_fail       ; catches a truncated 4 GiB+4 KiB -> 4 KiB alias
    mov eax, [high_original]
    mov [HIGH_VA], eax
    mov eax, [HIGH_VA]
    mov [high_restored], eax
    cmp eax, [high_original]
    jne .probe_fail
    mov eax, [LOW_ALIAS]
    cmp eax, [low_guard]
    jne .probe_fail
    mov byte [probe_ok], 1
    jmp .leave_pae
.restore_fail:
    ; Restore the original high dword also when the alias/read check failed.
    mov eax, [high_original]
    mov [HIGH_VA], eax
.probe_fail:
    mov byte [probe_ok], 0
.leave_pae:
    mov eax, [old_cr0]      ; PG off, PE stays on
    mov cr0, eax
    jmp .paging_off
.paging_off:
    mov eax, [old_cr3]
    mov cr3, eax
    mov eax, [old_cr4]
    mov cr4, eax
    cmp byte [probe_ok], 1
    jne error_probe
    mov esi, msg_probe_original
    call puts
    mov eax, [high_original]
    call print_hex32
    mov esi, msg_probe_pattern
    call puts
    mov eax, [high_pattern]
    call print_hex32
    mov esi, msg_probe_observed
    call puts
    mov eax, [high_observed]
    call print_hex32
    mov esi, msg_probe_low_before
    call puts
    mov eax, [low_guard]
    call print_hex32
    mov esi, msg_probe_low_during
    call puts
    mov eax, [low_guard_during]
    call print_hex32
    mov esi, msg_probe_restored
    call puts
    mov eax, [high_restored]
    call print_hex32
    mov esi, msg_newline
    call puts
    mov esi, msg_result
    call puts
    mov esi, msg_pass
    call puts
    mov eax, 0x10
    jmp qemu_exit

make_tables:
    ; Four 64-bit PDPTEs and four page directories. All low 4 GiB is
    ; identity-mapped in 2 MiB pages; one virtual slot is changed to phys 4 GiB.
    mov edi, PDPT
    mov ecx, 0x5000/4
    xor eax, eax
    rep stosd
    xor ebx, ebx
.pdpte:
    mov eax, ebx
    shl eax, 12
    add eax, PD_BASE
    or eax, 1              ; PDPTE present; other flag bits are not needed
    mov [PDPT+ebx*8], eax
    inc ebx
    cmp ebx, 4
    jb .pdpte
    xor ecx, ecx
.pde:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83           ; present, writable, 2 MiB page
    mov [PD_BASE+ecx*8], eax
    inc ecx
    cmp ecx, 2048
    jb .pde
    mov dword [PD_BASE+3*4096], 0x83       ; virtual C0000000 -> phys 4 GiB
    mov dword [PD_BASE+3*4096+4], 1        ; PDE physical bits above bit 31
    ret

exception_handler:
    ; #GP/#PF after entering PAE returns to a known, nonpaged print path.
    cli
    mov eax, cr0
    and eax, 0x7fffffff
    mov cr0, eax
    jmp .off
.off:
    mov eax, [old_cr3]
    mov cr3, eax
    mov eax, [old_cr4]
    mov cr4, eax
    mov esi, msg_exception
    call puts
    jmp error_probe

error_e820:
    mov esi, msg_error_e820
    jmp error_exit
error_cpu:
    mov esi, msg_error_cpu
    jmp error_exit
error_probe:
    mov esi, msg_error_probe
error_exit:
    call puts
    mov esi, msg_result
    call puts
    mov esi, msg_fail
    call puts
    mov eax, 0x11
qemu_exit:
    mov dx, 0xf4
    out dx, eax            ; QEMU isa-debug-exit: status (value << 1)|1
.halt:
    cli
    hlt
    jmp .halt

serial_init:
    mov dx, 0x3fb
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 1
    out dx, al
    mov dx, 0x3f9
    xor al, al
    out dx, al
    mov dx, 0x3fb
    mov al, 3
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

print_hex32:
    pushad
    mov ebx, eax
    mov ecx, 8
.next:
    mov eax, ebx
    shr eax, 28
    cmp al, 10
    jb .digit
    add al, 7
.digit:
    add al, '0'
    call putc
    shl ebx, 4
    dec ecx
    jnz .next
    popad
    ret

msg_banner db 'PAE STAGE: independent 32-bit probe', 13, 10, 0
msg_e820 db 'PAE E820: target=0x0000000100001000 usable_end=0x', 0
msg_paging db 'PAE PAGING: CR0.PG=1 CR4.PAE=1 PDE.phys_hi=1', 13, 10, 0
msg_probe_original db 'PAE PROBE original=0x', 0
msg_probe_pattern db ' pattern=0x', 0
msg_probe_observed db ' observed=0x', 0
msg_probe_low_before db ' low_before=0x', 0
msg_probe_low_during db ' low_during=0x', 0
msg_probe_restored db ' restored=0x', 0
msg_result db 'PAE RESULT physical=0x0000000100001000 ', 0
msg_pass db 'alias=clear restored=yes PASS', 13, 10, 0
msg_fail db 'FAIL', 13, 10, 0
msg_newline db 13, 10, 0
msg_error_e820 db 'PAE ERROR: no E820 type-1 span covers 4 GiB + 2 MiB', 13, 10, 0
msg_error_cpu db 'PAE ERROR: CPUID reports no PAE', 13, 10, 0
msg_error_probe db 'PAE ERROR: high-page read/write/restore check failed', 13, 10, 0
msg_exception db 'PAE EXCEPTION: paging disabled after GP/PF', 13, 10, 0

align 4
old_cr0 dd 0
old_cr3 dd 0
old_cr4 dd 0
low_guard dd 0
high_original dd 0
high_pattern dd 0
high_observed dd 0
high_restored dd 0
low_guard_during dd 0
e820_end_lo dd 0
e820_end_hi dd 0
e820_ok db 0
probe_ok db 0

align 8
idt:
    times 13 dq 0
    dw exception_handler, CODE_SEL
    db 0, 0x8e
    dw 0                    ; stage and handler live below physical 64 KiB
    dw exception_handler, CODE_SEL
    db 0, 0x8e
    dw 0
idt_end:
idt_descriptor:
    dw idt_end-idt-1
    dd idt

times 8192-($-$$) db 0
