; SPDX-License-Identifier: GPL-2.0-only
; Allocate 64 KiB at the 8 GiB virtual address, fill and verify all 8192 qwords,
; check a 64-bit checksum, release the allocation, and return a real exit code.
%include "abi.inc"
start:
    sub rsp, 0x58                   ; shadow space and stack arguments for ALLOC
    mov rax, 0x200000000
    mov [base], rax
    mov qword [size], 65536
    mov r10, SD64_CURRENT
    lea rdx, [base]
    xor r8d, r8d
    lea r9, [size]
    mov qword [rsp + 0x28], MEM_RESERVE_COMMIT
    mov qword [rsp + 0x30], PAGE_READWRITE
    mov eax, SD64_ALLOC
    syscall
    test eax, eax
    jnz .alloc_bad
    mov rbx, [base]
    mov rax, 0x200000000
    cmp rbx, rax
    jne .bad
    mov r12, 0x1122334455660000
    xor esi, esi
.fill:
    lea rax, [r12 + rsi]
    mov [rbx + rsi * 8], rax
    inc esi
    cmp esi, 8192
    jne .fill
    xor r13d, r13d
    xor esi, esi
.verify:
    lea rax, [r12 + rsi]
    mov rdx, [rbx + rsi * 8]
    cmp rdx, rax
    jne .bad
    add r13, rdx                    ; deliberately wraps modulo 2^64
    inc esi
    cmp esi, 8192
    jne .verify
    mov rax, r12
    shl rax, 13
    add rax, 33550336               ; 8192 * 8191 / 2
    cmp r13, rax
    jne .bad
    mov rax, rbx
    lea rdi, [address]
    call hex64
    mov rax, r13
    lea rdi, [checksum]
    call hex64
    call release
    test eax, eax
    jnz .release_bad
    PRINT message, message_end - message
    test eax, eax
    jnz .console_bad
    EXIT 0
.bad:
    call release
    PRINT verify_error, verify_error_end - verify_error
    EXIT 2
.alloc_bad:
    PRINT alloc_error, alloc_error_end - alloc_error
    EXIT 1
.release_bad:
    PRINT release_error, release_error_end - release_error
    EXIT 3
.console_bad:
    EXIT 4
release:
    mov qword [size], 0
    mov r10, SD64_CURRENT
    lea rdx, [base]
    lea r8, [size]
    mov r9d, MEM_RELEASE
    mov eax, SD64_FREE
    syscall
    ret
HEX64_ROUTINE
align 8
base: dq 0
size: dq 0
message: db 'MEM64: virtual base=0x'
address: times 16 db '0'
    db ' bytes=65536 checksum=0x'
checksum: times 16 db '0'
    db '; verify and release PASS', 10
message_end:
alloc_error: db 'MEM64: allocation failure', 10
alloc_error_end:
verify_error: db 'MEM64: address, pattern or checksum failure', 10
verify_error_end:
release_error: db 'MEM64: release failure', 10
release_error_end:
