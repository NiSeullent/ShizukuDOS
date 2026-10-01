; GPL-2.0-only. Standalone BIOS floppy loader for the read-only NVMe probe.
; Sectors 2..17 contain stage.asm at physical 0000:8000. No disk writes.
bits 16
org 0x7c00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    mov [boot_drive], dl
    mov bx, 0x8000
    mov ah, 0x02
    mov al, 16
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp al, 16
    jne disk_error
    jmp 0:0x8000

disk_error:
    mov si, error_text
.next:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    int 0x10
    jmp .next
.halt:
    cli
    hlt
    jmp .halt

boot_drive db 0
error_text db 'NVME PROBE BOOT ERROR',0
times 510-($-$$) db 0
dw 0xaa55
