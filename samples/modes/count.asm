; SPDX-License-Identifier: GPL-2.0-only
%include "mz_header.inc"
entry:
    mov ax, [cs:data_segment-body]
    mov ds, ax
    xor ax, ax
    mov cx, 1000
.sum:
    add ax, cx
    loop .sum
    cmp ax, (1000*1001/2) & 0xffff
    jne failed
    mov ah, 0x7f                    ; unknown DOS operation must carry error
    int 0x21
    jnc failed
    cmp ax, 1
    jne failed
    mov dx, message-body
    mov ah, 9
    int 0x21
    jc failed
    mov ax, 0x4c00
    int 0x21
failed:
    mov ax, 0x4c01
    int 0x21
message: db 'COUNT.EXE: 16-bit arithmetic + unsupported DOS-call error PASS',13,10,'$'
image_end:
