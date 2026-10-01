; SPDX-License-Identifier: GPL-2.0-only
%include "mz_header.inc"
entry:
    mov ax, [cs:data_segment-body]
    mov ds, ax
    mov dx, message-body
    mov ah, 9
    int 0x21
    jc failed
    mov ax, 0x4c00
    int 0x21
failed:
    mov ax, 0x4c01
    int 0x21
message: db 'HELLO.EXE: native DOS MZ relocation + INT21 text PASS', 13,10,'$'
image_end:
