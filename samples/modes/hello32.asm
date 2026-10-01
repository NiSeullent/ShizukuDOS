; SPDX-License-Identifier: GPL-2.0-only
%include "pe32_header.inc"
entry:
    mov eax, 1
    mov edx, message
    mov ecx, message_end-message
    int 0x80
    cmp eax, message_end-message
    jne failed
    xor eax, eax
    ret
failed:
    mov eax, 1
    ret
message: db 'HELLO32.EXE: real PE32/i386, kurazy int80 console PASS',13,10
message_end:
code_end:
raw_size equ ((code_end-code_start+511)/512)*512
times raw_size-($-code_start) db 0
