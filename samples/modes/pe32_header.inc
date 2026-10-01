; SPDX-License-Identifier: GPL-2.0-only
; An actual static PE32/i386 EXE, no imports, no Windows libraries.
bits 32
org 0x40000
section .header start=0x40000 vstart=0x40000 align=1
db 'MZ'
times 58 db 0
dd pe_header-$$
pe_header:
db 'PE',0,0
dw 0x014c, 1                      ; IMAGE_FILE_MACHINE_I386, one section
dd 0,0,0
dw 224, 0x0103                    ; PE32 optional header, relocs stripped
dw 0x010b
db 0,0
dd raw_size,0,0
dd entry-$$, 0x1000, 0
dd 0x40000, 4096, 512
dw 0,0,0,0,0,0
dd 0
dd 0x2000, 512, 0
dw 3, 0                           ; console subsystem, no Windows requirement
dd 0x10000,0x1000,0x10000,0x1000,0,16
times 16 dd 0,0
db '.text',0,0,0
dd code_end-code_start, 0x1000, raw_size, 512
dd 0,0
dw 0,0
dd 0xe0000020                     ; executable/read/write static section
times 512-($-$$) db 0
; Header occupies 512 file bytes, but the section executes at image+1000h.
section .text start=0x40200 vstart=0x41000 align=1
code_start:
