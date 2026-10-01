; SPDX-License-Identifier: GPL-2.0-only
; ShizukuDOS native SD64 ABI v1: flat x86-64 binary, entry 0x400000.
; SYSCALL clobbers RCX/R11/RAX. Arguments 1..4: R10/RDX/R8/R9;
; arguments 5+ at RSP+0x28, +0x30, ...; signed NTSTATUS in EAX.
bits 64
default abs
org 0x400000

SD64_EXIT     equ 0x01
SD64_ALLOC    equ 0x07
SD64_FREE     equ 0x08
SD64_PRINT    equ 0x30
SD64_CURRENT  equ -1
MEM_RESERVE_COMMIT equ 0x3000
MEM_RELEASE   equ 0x8000
PAGE_READWRITE equ 4

%macro PRINT 2
    mov r10, %1
    mov edx, %2
    mov eax, SD64_PRINT
    syscall
%endmacro

%macro EXIT 1
    mov r10, SD64_CURRENT
    mov edx, %1
    mov eax, SD64_EXIT
    syscall
    ud2                             ; exit must never return
%endmacro

; Convert unsigned RAX into exactly 16 hexadecimal digits at RDI.
; Clobbers RAX, RCX, RDX, RDI; no system call or hidden runtime dependency.
%macro HEX64_ROUTINE 0
hex64:
    mov ecx, 16
.digit:
    rol rax, 4
    mov edx, eax
    and edx, 15
    mov dl, [hex_digits + rdx]
    mov [rdi], dl
    inc rdi
    loop .digit
    ret
hex_digits: db '0123456789abcdef'
%endmacro
