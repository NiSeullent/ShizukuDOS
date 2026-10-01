; SPDX-License-Identifier: GPL-2.0-only
%include "pe32_header.inc"
entry:
    mov eax, 2
    mov edx, info
    int 0x80
    test eax, eax
    jnz failed
    cmp dword [info], 32
    jne failed
    mov eax, [info+4]
    and eax, 0x80000001
    cmp eax, 1
    jne failed
    test dword [info+8], 0x500
    jnz failed
    cmp dword [info+12], 0x18
    jne failed
    mov eax, cr0
    and eax, 0x80000001
    cmp eax, 1
    jne failed
    mov eax, 1
    mov edx, message
    mov ecx, message_end-message
    int 0x80
    xor eax, eax
    ret
failed:
    mov eax, 1
    ret
message: db 'MODE32.EXE: protected CR0 PE=1 PG=0, EFER LMA=0 PASS',13,10
message_end:
align 4
info: times 4 dd 0
code_end:
raw_size equ ((code_end-code_start+511)/512)*512
times raw_size-($-code_start) db 0
