; GPL-2.0-only. Disposable VirtualBox BIOS NVMe observation program.
; PCI config reads only; waits for a host-approved Y key before three MMIO
; reads. No PCI config writes, NVMe register writes, queues, DMA, or disk I/O.
bits 16
org 0x8000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    cld
    call serial_init
    mov si, start_text
    call puts16

    mov ax, 0xb101                 ; PCI BIOS installation check
    int 0x1a
    jc pci_error
    test ah, ah
    jnz pci_error
    cmp edx, 0x20494350           ; 'PCI '
    jne pci_error

    mov ax, 0xb103                 ; find first class 01:08:02
    mov ecx, 0x010802
    xor si, si
    int 0x1a
    jc no_nvme
    test ah, ah
    jnz no_nvme
    mov [bdf], bx

    xor di, di
    call pci_read32
    jc pci_error
    mov [pci_id], ecx
    mov di, 4
    call pci_read32
    jc pci_error
    mov [pci_command], ecx
    mov di, 8
    call pci_read32
    jc pci_error
    mov [pci_class], ecx
    mov di, 12
    call pci_read32
    jc pci_error
    mov [pci_header], ecx
    mov di, 16
    call pci_read32
    jc pci_error
    mov [bar0], ecx

    ; This stand-alone binary is intentionally bound to the VirtualBox model.
    ; A physical controller must not be MMIO-read merely by pressing Y.
    cmp dword [pci_id], 0x4e5680ee
    jne bad_pci
    mov eax, [pci_class]
    shr eax, 8
    cmp eax, 0x010802
    jne bad_pci
    test dword [pci_command], 2  ; memory decode must already be enabled
    jz bad_pci
    mov eax, [pci_header]
    shr eax, 16
    and eax, 0x7f
    jnz bad_pci                 ; endpoint header only
    mov eax, [bar0]
    test eax, 1
    jnz bad_pci                 ; BAR0 must be memory, not I/O
    mov edx, eax
    and edx, 6
    cmp edx, 0
    je .bar_ready
    cmp edx, 4
    jne bad_pci
    mov di, 20
    call pci_read32
    jc pci_error
    mov [bar1], ecx
    test ecx, ecx
    jnz bad_pci                 ; no proven >4-GiB mapper in this probe
.bar_ready:
    mov eax, [bar0]
    and eax, 0xfffffff0
    jz bad_pci
    mov [bar_base], eax

    mov si, ready_text
    call puts16
    movzx eax, word [bdf]
    call hex32_16
    mov si, id_text
    call puts16
    mov eax, [pci_id]
    call hex32_16
    mov si, class_text
    call puts16
    mov eax, [pci_class]
    call hex32_16
    mov si, cmd_text
    call puts16
    mov eax, [pci_command]
    call hex32_16
    mov si, bar_text
    call puts16
    mov eax, [bar_base]
    call hex32_16
    mov si, line_end
    call puts16

    ; The host must first cross-check this BDF/BAR against VBox debugvm's
    ; MMIO region #0. Without explicit approval no MMIO address is accessed.
    xor ah, ah
    int 0x16
    cmp al, 'Y'
    jne denied
    mov si, approval_text
    call puts16
    cli
    in al, 0x92                  ; enable A20 for flat physical MMIO mapping
    or al, 2
    and al, 0xfe
    out 0x92, al
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:protected

pci_read32:
    mov bx, [bdf]
    mov ax, 0xb10a
    int 0x1a
    jc .error
    test ah, ah
    jnz .error
    clc
    ret
.error:
    stc
    ret

pci_error:
    mov si, pci_error_text
    jmp fail16
no_nvme:
    mov si, no_nvme_text
    jmp fail16
bad_pci:
    mov si, bad_pci_text
    jmp fail16
denied:
    mov si, denied_text
fail16:
    call puts16
.halt:
    cli
    hlt
    jmp .halt

serial_init:
    mov dx, 0x3fb
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 1
    out dx, al
    mov dx, 0x3f9
    xor al, al
    out dx, al
    mov dx, 0x3fb
    mov al, 3
    out dx, al
    mov dx, 0x3fa
    mov al, 0xc7
    out dx, al
    mov dx, 0x3fc
    mov al, 3
    out dx, al
    ret

puts16:
    lodsb
    test al, al
    jz .done
    call putc16
    jmp puts16
.done:
    ret

putc16:
    push eax
    push ecx
    push edx
    mov ah, al
    mov dx, 0x3fd
    mov ecx, 100000
.ready:
    in al, dx
    test al, 0x20
    jnz .send
    dec ecx
    jnz .ready
.send:
    mov al, ah
    mov dx, 0x3f8
    out dx, al
    pop edx
    pop ecx
    pop eax
    ret

hex32_16:
    pushad
    mov ebx, eax
    mov cx, 8
.next:
    mov eax, ebx
    shr eax, 28
    call nibble16
    shl ebx, 4
    loop .next
    popad
    ret
nibble16:
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    call putc16
    ret

align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
gdt_end:
gdt_desc:
    dw gdt_end-gdt-1
    dd gdt

bits 32
protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7c00
    mov ebx, [bar_base]
    mov eax, [ebx]             ; NVMe CAP low (read only)
    mov [cap_low], eax
    mov eax, [ebx+4]           ; NVMe CAP high (read only)
    mov [cap_high], eax
    mov eax, [ebx+8]           ; NVMe VS (read only)
    mov [version], eax

    mov esi, mmio_text
    call puts32
    mov eax, [cap_high]
    call hex32_32
    mov al, ':'
    call putc32
    mov eax, [cap_low]
    call hex32_32
    mov esi, vs_text
    call puts32
    mov eax, [version]
    call hex32_32
    mov esi, line_end
    call puts32
    mov eax, [cap_low]
    cmp eax, 0xffffffff
    je bad_mmio
    test ax, ax
    jz bad_mmio
    mov eax, [version]
    test eax, eax
    jz bad_mmio
    cmp eax, 0xffffffff
    je bad_mmio
    mov esi, pass_text
    jmp finish32
bad_mmio:
    mov esi, bad_mmio_text
finish32:
    call puts32
.halt:
    cli
    hlt
    jmp .halt

puts32:
    lodsb
    test al, al
    jz .done
    call putc32
    jmp puts32
.done:
    ret
putc32:
    push eax
    push ecx
    push edx
    mov ah, al
    mov dx, 0x3fd
    mov ecx, 100000
.ready:
    in al, dx
    test al, 0x20
    jnz .send
    dec ecx
    jnz .ready
.send:
    mov al, ah
    mov dx, 0x3f8
    out dx, al
    pop edx
    pop ecx
    pop eax
    ret
hex32_32:
    pushad
    mov ebx, eax
    mov ecx, 8
.next:
    mov eax, ebx
    shr eax, 28
    call nibble32
    shl ebx, 4
    loop .next
    popad
    ret
nibble32:
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    call putc32
    ret

bits 16
bdf dw 0
pci_id dd 0
pci_command dd 0
pci_class dd 0
pci_header dd 0
bar0 dd 0
bar1 dd 0
bar_base dd 0
cap_low dd 0
cap_high dd 0
version dd 0
start_text db 'NVME_BOOT BIOS READ_ONLY',13,10,0
ready_text db 'NVME_READY BDF=',0
id_text db ' ID=',0
class_text db ' CLASS=',0
cmd_text db ' CMD=',0
bar_text db ' BAR=',0
approval_text db 'NVME_HOST_APPROVED',13,10,0
mmio_text db 'NVME_MMIO CAP=',0
vs_text db ' VS=',0
pass_text db 'NVME_RESULT PASS',13,10,0
pci_error_text db 'NVME_RESULT FAIL PCI_BIOS',13,10,0
no_nvme_text db 'NVME_RESULT FAIL NO_DEVICE',13,10,0
bad_pci_text db 'NVME_RESULT FAIL BAD_PCI',13,10,0
denied_text db 'NVME_RESULT FAIL HOST_DENIED',13,10,0
bad_mmio_text db 'NVME_RESULT FAIL BAD_MMIO',13,10,0
line_end db 13,10,0

times 8192-($-$$) db 0
