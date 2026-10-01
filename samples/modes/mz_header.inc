; SPDX-License-Identifier: GPL-2.0-only
; Standard DOS MZ, two-paragraph header, one relocation proving loader support.
bits 16
org 0
db 'MZ'
dw (image_end-$$) % 512
dw ((image_end-$$)+511)/512
dw 1
dw 2
dw 0x1000, 0xffff                  ; additional memory, full conventional arena
dw 0, 0xfff0                     ; SS:SP relative to load segment
dw 0                             ; checksum
dw entry-body, 0                 ; CS:IP relative to load segment
dw 28, 0                         ; relocation table, no overlay
dw data_segment-body, 0
body:
    jmp entry
data_segment: dw 0                ; DOS loader adds image segment 2010h
