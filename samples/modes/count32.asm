; SPDX-License-Identifier: GPL-2.0-only
%include "pe32_header.inc"
entry:
    mov eax, 100000
    mov ecx, 100000
    mul ecx                         ; actual 32-bit arithmetic produces 64-bit pair
    cmp edx, 2
    jne failed
    cmp eax, 0x540be400
    jne failed
    mov eax, 99                     ; unsupported kurazy32 call must fail
    int 0x80
    cmp eax, 0xffffffff
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
message: db 'COUNT32.EXE: 100000*100000=10000000000, ABI error PASS',13,10
message_end:
code_end:
raw_size equ ((code_end-code_start+511)/512)*512
times raw_size-($-code_start) db 0
