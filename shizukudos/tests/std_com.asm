; Runs INT 21h/AH=09h with DF=1. DOS must print forward and preserve caller DF.
bits 16
org 0x0100

    std
    mov dx, forward_text
    mov ah, 0x09
    int 0x21
    pushf
    pop ax
    test ax, 0x0400
    jz failed
    mov ax, 0x4c33       ; exit with DF still set; shell must clear it
    int 0x21
failed:
    mov dx, failed_text
    mov ah, 0x09
    int 0x21
    mov ax, 0x4c01
    int 0x21

forward_text: db 'STD forward string OK',13,10,'$'
failed_text: db 'STD caller DF was lost',13,10,'$'
