; SPDX-License-Identifier: GPL-2.0-only
; Self-contained native CPU transition island copied to physical 00010000.
; All bridge addresses below are physical constants, independent of ELF VMA.
; Interrupts remain masked: these are trusted finite compatibility samples,
; not a preemptive DOS virtual machine or a DPMI host.
%define BASE 0x10000
%define CTL  0x18000
%define S_GDTR (CTL+0x2000)
%define S_IDTR (CTL+0x2010)
%define S_RSP (CTL+0x2020)
%define S_CR0 (CTL+0x2030)
%define S_CR3 (CTL+0x2038)
%define S_CR4 (CTL+0x2040)
%define S_EFER (CTL+0x2048)
%define S_PIC (CTL+0x2050)
%define S_FS (CTL+0x2060)
%define S_GS (CTL+0x2068)
%define S_SEG (CTL+0x2070)
%define S_KGS (CTL+0x2078)
%define S_DSEG (CTL+0x2080)
%define PHY(x) (BASE + x - cpu_modes_bridge_start)
%define OFF(x) (x - cpu_modes_bridge_start)

section .rodata align=16
bits 64
default abs
global cpu_modes_bridge_start, cpu_modes_bridge_end
global cpu_modes_rm_int21_offset, cpu_modes_rm_exit_offset, cpu_modes_rm_fault_offset
global cpu_modes_pm_int80_offset, cpu_modes_pm_fault_offset
cpu_modes_bridge_start:
    pushfq
    cli
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [abs S_RSP], rsp
    sgdt [abs S_GDTR]
    sidt [abs S_IDTR]
    mov rax, cr0
    mov [abs S_CR0], rax
    mov rax, cr3
    mov [abs S_CR3], rax
    mov rax, cr4
    mov [abs S_CR4], rax
    mov ecx, 0xc0000080
    rdmsr
    mov [abs S_EFER], eax
    mov [abs S_EFER+4], edx
    mov ecx, 0xc0000100
    rdmsr
    mov [abs S_FS], eax
    mov [abs S_FS+4], edx
    mov ecx, 0xc0000101
    rdmsr
    mov [abs S_GS], eax
    mov [abs S_GS+4], edx
    mov ecx, 0xc0000102
    rdmsr
    mov [abs S_KGS], eax
    mov [abs S_KGS+4], edx
    mov ax, fs
    mov [abs S_SEG], ax
    mov ax, gs
    mov [abs S_SEG+2], ax
    mov ax, ds
    mov [abs S_DSEG], ax
    mov ax, es
    mov [abs S_DSEG+2], ax
    mov ax, ss
    mov [abs S_DSEG+4], ax
    in al, 0x21
    mov [abs S_PIC], al
    in al, 0xa1
    mov [abs S_PIC+1], al
    mov al, 0xff
    out 0x21, al
    out 0xa1, al
    in al, 0x70
    mov [abs S_PIC+2], al
    or al, 0x80
    out 0x70, al
    lgdt [abs PHY(bridge_gdtr)]
    mov rsp, 0x17ff0
    push qword 0x18
    push qword PHY(protected_entry)
    retfq

bits 32
protected_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x17ff0
    mov eax, cr0
    and eax, 0x7fffffff
    mov cr0, eax                     ; leave IA-32e, LMA really becomes zero
    mov ecx, 0xc0000080
    rdmsr
    and eax, 0xfffffeff
    wrmsr                            ; LME is also zero while legacy apps run
    lidt [PHY(pm_idtr)]
    mov eax, cr0
    mov [CTL+40], eax
    mov ecx, 0xc0000080
    rdmsr
    mov [CTL+44], eax
    xor eax, eax
    mov ax, cs
    mov [CTL+48], eax
    cmp dword [CTL], 16
    je enter_real
    mov esp, 0x6fff0
    call dword [CTL+24]
    mov [CTL+4], eax
    mov dword [CTL+8], 32
    jmp protected_return

enter_real:
    jmp word 0x20:OFF(protected16_entry)
bits 16
protected16_entry:
    mov ax, 0x28
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7ff0
    mov eax, cr0
    and eax, 0xfffffffe
    mov cr0, eax                     ; native Real Mode, PE=0 and PG=0
    jmp 0x1000:OFF(real_entry)

real_entry:
    mov ax, 0x1000
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7ff0
    lidt [cs:OFF(rm_idtr)]
    mov eax, cr0
    mov [cs:0x801c], eax
    mov ecx, 0xc0000080
    rdmsr
    mov [cs:0x8020], eax
    xor eax, eax
    mov ax, cs
    mov [cs:0x8024], eax
    mov ax, [cs:0x8010]
    mov ss, ax
    mov sp, [cs:0x8012]
    mov ax, [cs:0x8014]
    mov ds, ax
    mov es, ax
    push word [cs:0x800c]
    push word [cs:0x800e]
    retf                            ; DOS MZ CS:IP, DS/ES point at a PSP

real_exit:
    xor eax, eax
    mov [cs:0x8004], eax
    jmp real_return
real_fault:
    mov dword [cs:0x803c], 1
    mov dword [cs:0x8004], 0xffffffff
    jmp real_return
real_return:
    cli
    cld
    mov ax, 0x1000
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7ff0
    mov dword [cs:0x8008], 16
    mov eax, [cs:(S_CR0-BASE)]
    and eax, 0x7fffffff
    or eax, 1
    mov cr0, eax
    jmp dword 0x18:PHY(protected_return)

; AH=02/09/30/40/4c and F0, with standard carry/error reporting.
real_int21:
    cmp ah, 0x4c
    jne .service
    movzx eax, al
    mov [cs:0x8004], eax
    jmp real_return
.service:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    mov bp, sp
    mov bx, ds
    mov es, bx
    mov bx, 0x1800
    mov ds, bx
    movzx ebx, word [ss:bp+18]
    mov [36], ebx                  ; observed MZ application's interrupt CS
    and word [ss:bp+20], 0xfffe
    cmp ah, 2
    je .char
    cmp ah, 9
    je .dollar
    cmp ah, 0x30
    je .version
    cmp ah, 0x40
    je .write
    cmp ah, 0xf0
    je .query
.unsupported:
    mov ax, 1
    or word [ss:bp+20], 1
    jmp .done
.version:
    mov ax, 5                       ; DOS-compatible API subset version 5.0
    mov word [ss:bp+14], 0
    jmp .done
.query:
    mov ax, 16
    mov dx, [0x1c]
    mov [ss:bp+10], dx
    jmp .done
.char:
    mov al, dl
    call .emit
    jmp .done
.dollar:
    mov si, dx
    mov cx, 1024
.next:
    call .valid_pointer
    jc .unsupported
    mov al, [es:si]
    inc si
    cmp al, '$'
    je .dollar_done
    call .emit
    loop .next
    jmp .unsupported
.dollar_done:
    mov al, '$'
    jmp .done
.write:
    cmp word [ss:bp+14], 1
    je .write_ok
    cmp word [ss:bp+14], 2
    jne .unsupported
.write_ok:
    cmp cx, 1024
    ja .unsupported
    mov si, dx
    mov di, cx
    test cx, cx
    jz .written
.write_loop:
    call .valid_pointer
    jc .unsupported
    mov al, [es:si]
    inc si
    call .emit
    loop .write_loop
.written:
    mov ax, di
    jmp .done
.valid_pointer:
    push eax
    push edx
    mov ax, es
    movzx eax, ax
    shl eax, 4
    movzx edx, si
    add eax, edx
    cmp eax, 0x20000
    jb .pointer_bad
    cmp eax, 0x40000
    jae .pointer_bad
    pop edx
    pop eax
    clc
    ret
.pointer_bad:
    pop edx
    pop eax
    stc
    ret
.emit:
    push bx
    mov bx, [56]
    cmp bx, 1023
    jae .full
    mov [bx+64], al
    inc bx
    mov [56], bx
.full:
    pop bx
    ret
.done:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    iret

bits 32
protected_fault:
    mov dword [CTL+60], 1
    mov dword [CTL+4], 0xffffffff
    mov dword [CTL+8], 32
    jmp protected_return
protected_int80:
    pushad
    cld
    cmp eax, 1
    je .write
    cmp eax, 2
    je .query
    cmp eax, 3
    je .yield
    mov dword [esp+28], 0xffffffff
    jmp .done
.write:
    cmp edx, 0x40000
    jb .invalid
    cmp ecx, 1024
    ja .invalid
    mov eax, edx
    add eax, ecx
    jc .invalid
    cmp eax, 0x70000
    ja .invalid
    mov esi, edx
    mov ebx, [CTL+56]
    mov edx, ecx
.write_loop:
    test ecx, ecx
    jz .written
    cmp ebx, 1023
    jae .written
    lodsb
    mov [CTL+64+ebx], al
    inc ebx
    dec ecx
    jmp .write_loop
.written:
    mov [CTL+56], ebx
    sub edx, ecx
    mov [esp+28], edx
    jmp .done
.query:
    cmp edx, 0x40000
    jb .invalid
    cmp edx, 0x6fff0
    ja .invalid
    mov dword [edx], 32
    mov eax, cr0
    mov [edx+4], eax
    mov ecx, 0xc0000080
    rdmsr
    mov edx, [esp+20]              ; PUSHAD saved EDX
    mov [edx+8], eax
    xor eax, eax
    mov ax, cs
    mov [edx+12], eax
.yield:
    mov dword [esp+28], 0
    jmp .done
.invalid:
    mov dword [esp+28], 0xffffffff
.done:
    popad
    iretd

protected_return:
    cli
    cld
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x17ff0
    mov ecx, 0xc0000080
    mov eax, [S_EFER]
    mov edx, [S_EFER+4]
    wrmsr
    mov eax, [S_CR4]
    mov cr4, eax
    mov eax, [S_CR3]
    mov cr3, eax
    mov eax, [S_CR0]
    mov cr0, eax
    jmp 0x08:PHY(long_return)

bits 64
long_return:
    lgdt [abs S_GDTR]
    lidt [abs S_IDTR]
    mov ax, [abs S_DSEG]
    mov ds, ax
    mov ax, [abs S_DSEG+2]
    mov es, ax
    mov ax, [abs S_DSEG+4]
    mov ss, ax
    mov ax, [abs S_SEG]
    mov fs, ax
    mov ax, [abs S_SEG+2]
    mov gs, ax
    mov ecx, 0xc0000100
    mov eax, [abs S_FS]
    mov edx, [abs S_FS+4]
    wrmsr
    mov ecx, 0xc0000101
    mov eax, [abs S_GS]
    mov edx, [abs S_GS+4]
    wrmsr
    mov ecx, 0xc0000102
    mov eax, [abs S_KGS]
    mov edx, [abs S_KGS+4]
    wrmsr
    mov al, [abs S_PIC]
    out 0x21, al
    mov al, [abs S_PIC+1]
    out 0xa1, al
    mov al, [abs S_PIC+2]
    out 0x70, al
    mov rsp, [abs S_RSP]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    ret

align 8
bridge_gdt:
    dq 0
    dq 0x00af9b000000ffff            ; 08 Long64 code
    dq 0x00cf93000000ffff            ; 10 flat data
    dq 0x00cf9b000000ffff            ; 18 Protected32 code
    dq 0x00009b010000ffff            ; 20 Protected16 code, base10000
    dq 0x000093010000ffff            ; 28 Protected16 data, base10000
bridge_gdtr: dw (6*8)-1
    dq PHY(bridge_gdt)
pm_idtr: dw 2047
    dd CTL+0xc00
rm_idtr: dw 1023
    dd CTL+0x800

; C obtains offsets from tiny words, without source-generated include files.
cpu_modes_rm_int21_offset: dd OFF(real_int21)
cpu_modes_rm_exit_offset: dd OFF(real_exit)
cpu_modes_rm_fault_offset: dd OFF(real_fault)
cpu_modes_pm_int80_offset: dd OFF(protected_int80)
cpu_modes_pm_fault_offset: dd OFF(protected_fault)
cpu_modes_bridge_end:
section .note.GNU-stack noalloc noexec nowrite progbits
