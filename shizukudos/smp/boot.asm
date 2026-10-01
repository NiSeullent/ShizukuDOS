; Standalone 1.44 MB floppy boot sector. This is not the ShizukuDOS boot path.
; Stage 2 occupies sectors 2..17 at 0000:8000. AP trampoline is sector 18
; at 0000:7000, making SIPI vector 07h.
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

    mov bx, 0x7000
    mov ah, 0x02
    mov al, 1
    mov ch, 0
    mov cl, 18
    mov dh, 0
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp al, 1
    jne disk_error

    jmp 0x0000:0x8000

disk_error:
    mov si, disk_message
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

boot_drive db 0
disk_message db 'SMP BOOT READ ERROR', 0

times 510-($-$$) db 0
dw 0xaa55
