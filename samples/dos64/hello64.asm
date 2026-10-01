; SPDX-License-Identifier: GPL-2.0-only
; A native x86-64 arithmetic and console example, not a Win64 PE executable.
%include "abi.inc"
start:
    mov eax, 100000000
    mov ebx, 100000000
    mul rbx                         ; unsigned 64-bit RDX:RAX product
    test rdx, rdx
    jnz .bad
    mov rbx, 10000000000000000
    cmp rax, rbx
    jne .bad
    lea rdi, [product]
    call hex64
    PRINT message, message_end - message
    test eax, eax
    jnz .bad
    EXIT 0
.bad:
    PRINT error, error_end - error
    EXIT 1
HEX64_ROUTINE
message: db 'HELLO64: 100000000 * 100000000 = 0x'
product: times 16 db '0'
    db ' (10000000000000000); 64-bit arithmetic PASS', 10
message_end:
error: db 'HELLO64: arithmetic or console failure', 10
error_end:
