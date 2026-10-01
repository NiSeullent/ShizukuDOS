; SPDX-License-Identifier: GPL-2.0-only
%include "mz_header.inc"
entry:
    mov ax, [cs:data_segment-body]
    mov ds, ax
    mov ah, 0xf0
    int 0x21
    jc failed
    cmp ax, 16
    jne failed
    test dx, 1                     ; CPU really is outside protected mode
    jnz failed
    mov eax, cr0
    test eax, 0x80000001
    jnz failed
    mov ah, 0x30
    int 0x21
    cmp al, 5
    jne failed
    mov dx, message-body
    mov cx, message_end-message
    mov bx, 1
    mov ah, 0x40
    int 0x21
    jc failed
    cmp ax, message_end-message
    jne failed
    mov ax, 0x4c00
    int 0x21
failed:
    mov ax, 0x4c01
    int 0x21
message: db 'MODE.EXE: CR0 PE=0 PG=0, DOS version/write compatibility PASS',13,10
message_end:
image_end:
