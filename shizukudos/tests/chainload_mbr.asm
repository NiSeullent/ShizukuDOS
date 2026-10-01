; Test-only MBR. Prints a marker to COM1, then stops.
bits 16
org 0x7c00

start:
    cmp dl, 0x80
    jne wrong_handoff
    mov ax, ds
    test ax, ax
    jnz wrong_handoff
    mov ax, es
    test ax, ax
    jnz wrong_handoff
    mov ax, ss
    test ax, ax
    jnz wrong_handoff
    cmp sp, 0x7c00
    jne wrong_handoff
    pushf
    pop ax
    test ax, 0x0200
    jz wrong_handoff
    xor ax, ax
    mov ds, ax
    mov si, message
    jmp print_message
wrong_handoff:
    xor ax, ax
    mov ds, ax
    mov si, wrong_handoff_message
print_message:
.next:
    lodsb
    test al, al
    jz .stop
    mov ah, al
.wait:
    mov dx, 0x3fd
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3f8
    mov al, ah
    out dx, al
    jmp .next
.stop:
    cli
    hlt
    jmp .stop

message: db 'CHAINLOAD MBR OK',13,10,0
wrong_handoff_message: db 'CHAINLOAD MBR BAD HANDOFF',13,10,0

times 446-($-$$) db 0
; One active partition whose VBR is at LBA 1 / CHS 0:0:2.
db 0x80, 0x00, 0x02, 0x00, 0x06, 0x00, 0x02, 0x00
dd 1, 2047
times 3*16 db 0
dw 0xaa55
