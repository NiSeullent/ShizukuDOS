; Minimal ShizukuDOS-compatible .COM program. Loaded at PSP:0100h.
bits 16
org 0x0100

    mov dx, greeting
    mov ah, 0x09
    int 0x21
    mov dl, '!'
    mov ah, 0x02
    int 0x21
    mov dx, newline
    mov ah, 0x09
    int 0x21
    mov ah, 0x30
    int 0x21
    cmp ax, 0x0100
    jne failed
    mov ah, 0xfe             ; deliberately unsupported function
    int 0x21
    jnc failed
    cmp ax, 1
    jne failed
    mov dx, checks_ok
    mov ah, 0x09
    int 0x21
    mov ax, 0x4c2a
    int 0x21
failed:
    mov dx, checks_failed
    mov ah, 0x09
    int 0x21
    mov ax, 0x4c01
    int 0x21

greeting: db 'Hello from a ShizukuDOS COM program',13,10,'INT 21h calls 09h and 02h: $'
newline: db 13,10,'$'
checks_ok: db 'INT 21h version/error checks OK',13,10,'$'
checks_failed: db 'INT 21h API check FAILED',13,10,'$'
