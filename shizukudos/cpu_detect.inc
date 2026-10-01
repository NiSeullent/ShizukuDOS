; ShizukuDOS real-mode CPU capability and package-topology discovery.
; Copyright (C) 2026 Win98-Modern contributors. GPL-2.0-only.
;
; 386-safe entry: EFLAGS.AC distinguishes 386/486, then EFLAGS.ID guards
; every CPUID instruction. XGETBV is executed only when CPUID advertises
; XSAVE, OSXSAVE and AVX. This is a read-only inventory: no AP startup,
; CR4 changes, protected-mode transition, or scheduler is attempted.
;
; Intel SDM Vol. 2 CPUID/XGETBV and AMD APM Vol. 3 CPUID define the bit and
; topology leaf layout. Source notes: shizukudos/CPU_DISCOVERY.md.

cpu_detect:
    pushfd
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov byte [cpu_has_cpuid], 0
    mov byte [cpu_legacy_class], 3
    mov word [cpu_logical], 1
    mov word [cpu_cores], 1
    mov word [cpu_threads], 1
    mov byte [cpu_top_source], 0
    mov byte [cpu_avx_eligible], 0
    mov dword [cpu_leaf1_ecx], 0
    mov dword [cpu_leaf1_edx], 0
    mov dword [cpu_max_basic], 0
    mov dword [cpu_max_ext], 0

    ; 80386 cannot toggle AC, while an 80486 can. Restore EFLAGS before
    ; testing ID, and restore the caller's original EFLAGS at routine exit.
    pushfd
    pop eax
    mov esi, eax
    xor eax, 0x00040000
    push eax
    popfd
    pushfd
    pop eax
    xor eax, esi
    test eax, 0x00040000
    jz .no_ac
    mov byte [cpu_legacy_class], 4
.no_ac:
    push esi
    popfd

    pushfd
    pop eax
    mov esi, eax
    xor eax, 0x00200000
    push eax
    popfd
    pushfd
    pop eax
    xor eax, esi
    test eax, 0x00200000
    jz .no_cpuid
    push esi
    popfd
    mov byte [cpu_has_cpuid], 1

    xor eax, eax
    cpuid
    mov [cpu_max_basic], eax
    mov [cpu_vendor], ebx
    mov [cpu_vendor+4], edx
    mov [cpu_vendor+8], ecx
    cmp dword [cpu_max_basic], 1
    jb .extended_leaf
    mov eax, 1
    cpuid
    mov [cpu_signature], eax
    mov [cpu_leaf1_ecx], ecx
    mov [cpu_leaf1_edx], edx
    mov esi, ebx

    ; CPUID signature: extended family applies only to base family 15;
    ; extended model applies only to base family 6 or 15.
    mov ebx, eax
    shr eax, 8
    and eax, 15
    mov [cpu_base_family], ax
    mov [cpu_family], ax
    cmp ax, 15
    jne .base_model
    mov eax, ebx
    shr eax, 20
    and ax, 255
    add [cpu_family], ax
.base_model:
    mov eax, ebx
    shr eax, 4
    and ax, 15
    mov [cpu_model], ax
    cmp word [cpu_base_family], 6
    je .extended_model
    cmp word [cpu_base_family], 15
    jne .stepping
.extended_model:
    mov eax, ebx
    shr eax, 16
    and ax, 15
    shl ax, 4
    add [cpu_model], ax
.stepping:
    and bx, 15
    mov [cpu_stepping], bx

    ; Leaf 1 gives package logical count only if HTT is advertised. It
    ; cannot by itself split SMT threads from cores, so mark that unknown.
    bt dword [cpu_leaf1_edx], 28
    jnc .check_avx
    mov eax, esi
    shr eax, 16
    and ax, 255
    test ax, ax
    jz .check_avx
    mov [cpu_logical], ax
    cmp ax, 1
    jbe .check_avx
    mov word [cpu_cores], 0

.check_avx:
    mov eax, [cpu_leaf1_ecx]
    and eax, 0x1c000000     ; XSAVE, OSXSAVE, AVX all required
    cmp eax, 0x1c000000
    jne .extended_leaf
    mov eax, cr0
    test eax, 0x0000000c     ; EM or TS would trap vector/SIMD use
    jnz .extended_leaf
    xor ecx, ecx            ; XCR0
    xgetbv
    and eax, 6             ; XMM and YMM state both enabled
    cmp eax, 6
    jne .extended_leaf
    mov byte [cpu_avx_eligible], 1

.extended_leaf:
    mov eax, 0x80000000
    cpuid                   ; safe because ID toggle already succeeded
    cmp eax, 0x80000000
    jb .topology
    mov [cpu_max_ext], eax

.topology:
    cmp dword [cpu_max_basic], 0x1f
    jb .try_leaf_b
    mov dword [cpu_top_leaf], 0x1f
    mov byte [cpu_top_candidate], 2
    call cpu_probe_topology
    cmp byte [cpu_top_source], 0
    jne .done
.try_leaf_b:
    cmp dword [cpu_max_basic], 0x0b
    jb .try_leaf_4
    mov dword [cpu_top_leaf], 0x0b
    mov byte [cpu_top_candidate], 1
    call cpu_probe_topology
    cmp byte [cpu_top_source], 0
    jne .done

.try_leaf_4:
    ; Intel deterministic-cache leaf 4 encodes cores per package. A
    ; hypervisor may mask it; accept only consistent, divisible counts.
    cmp dword [cpu_max_basic], 4
    jb .try_amd_8
    mov eax, 4
    xor ecx, ecx
    cpuid
    mov ebx, eax
    and ebx, 31
    test ebx, ebx
    jz .try_amd_8
    shr eax, 26
    and ax, 63
    inc ax
    mov cx, ax
    cmp cx, [cpu_logical]
    ja .try_amd_8
    mov ax, [cpu_logical]
    xor dx, dx
    div cx
    test dx, dx
    jnz .try_amd_8
    mov [cpu_threads], ax
    mov [cpu_cores], cx
    mov byte [cpu_top_source], 3
    jmp .done

.try_amd_8:
    ; AMD's extended leaf 80000008h reports NC (cores - 1) in ECX[7:0].
    ; Do not interpret Intel's reserved ECX bits as an AMD core count.
    cmp byte [cpu_vendor], 'A'
    jne .done
    cmp byte [cpu_vendor+1], 'u'
    jne .done
    cmp byte [cpu_vendor+2], 't'
    jne .done
    cmp byte [cpu_vendor+3], 'h'
    jne .done
    cmp dword [cpu_max_ext], 0x80000008
    jb .done
    mov eax, 0x80000008
    cpuid
    and ecx, 255
    inc cx
    cmp cx, [cpu_logical]
    ja .done
    mov ax, [cpu_logical]
    xor dx, dx
    div cx
    test dx, dx
    jnz .done
    mov [cpu_threads], ax
    mov [cpu_cores], cx
    mov byte [cpu_top_source], 4
    jmp .done

.no_cpuid:
    push esi
    popfd
.done:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    popfd
    ret

; Called only after a maximum-basic-leaf check. A valid core level reports
; package logical processors (EBX[15:0]); SMT level reports threads/core.
; Enumerate at most eight levels so a broken firmware/hypervisor cannot hang.
cpu_probe_topology:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi
    mov word [cpu_top_logical], 0
    mov word [cpu_threads], 1
    mov byte [cpu_top_core_seen], 0
    xor si, si
.next:
    mov eax, [cpu_top_leaf]
    movzx ecx, si
    cpuid
    test bx, bx
    jz .finish
    mov edi, ecx
    shr edi, 8
    and edi, 255
    test edi, edi
    jz .finish
    ; The highest valid hierarchy level may be Module/Tile/Die (leaf 1Fh),
    ; so keep its EBX count as the package-wide logical count. A Core level
    ; must still be present before we trust the hierarchy.
    mov [cpu_top_logical], bx
    cmp edi, 1
    jne .core_level
    mov [cpu_threads], bx
    jmp .advance
.core_level:
    cmp edi, 2
    jne .advance
    mov byte [cpu_top_core_seen], 1
.advance:
    inc si
    cmp si, 8
    jb .next
.finish:
    cmp byte [cpu_top_core_seen], 1
    jne .invalid
    mov ax, [cpu_top_logical]
    test ax, ax
    jz .invalid
    cmp ax, [cpu_threads]
    jb .invalid
    xor dx, dx
    div word [cpu_threads]
    test ax, ax
    jz .invalid
    test dx, dx
    jnz .invalid
    mov [cpu_cores], ax
    mov ax, [cpu_top_logical]
    mov [cpu_logical], ax
    mov al, [cpu_top_candidate]
    mov [cpu_top_source], al
    jmp .return
.invalid:
    mov word [cpu_threads], 1
.return:
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

cpu_has_cpuid: db 0
cpu_legacy_class: db 3
cpu_vendor: times 12 db 0
    db 0
cpu_max_basic: dd 0
cpu_max_ext: dd 0
cpu_signature: dd 0
cpu_leaf1_ecx: dd 0
cpu_leaf1_edx: dd 0
cpu_base_family: dw 0
cpu_family: dw 0
cpu_model: dw 0
cpu_stepping: dw 0
cpu_logical: dw 1
cpu_cores: dw 1
cpu_threads: dw 1
cpu_top_leaf: dd 0
cpu_top_logical: dw 0
cpu_top_core_seen: db 0
cpu_top_candidate: db 0
cpu_top_source: db 0
cpu_avx_eligible: db 0

cpu_header_text: db 'CPU discovery (read-only CPUID hints):',13,10,0
cpu_legacy_text: db 'CPUID unavailable; CPU class i',0
cpu_legacy_suffix: db '86; topology and AVX unknown.',13,10,0
cpu_vendor_text: db 'Vendor: ',0
cpu_leaf1_unavailable_text: db 'CPUID leaf 1 unavailable; family, topology and features unknown.',13,10,0
cpu_family_text: db 'Family ',0
cpu_model_text: db ' model ',0
cpu_stepping_text: db ' stepping ',0
cpu_logical_text: db 'Package logical ',0
cpu_cores_text: db ' cores ',0
cpu_threads_text: db ' threads/core ',0
cpu_unknown_text: db 'unknown',0
cpu_top_source_text: db 'Topology source: ',0
cpu_top_source_labels:
    dw cpu_top_leaf1_text, cpu_top_leafb_text, cpu_top_leaf1f_text
    dw cpu_top_leaf4_text, cpu_top_amd8_text
cpu_top_leaf1_text: db 'leaf 1 count only',0
cpu_top_leafb_text: db 'leaf 0Bh',0
cpu_top_leaf1f_text: db 'leaf 1Fh',0
cpu_top_leaf4_text: db 'leaf 4 cache fallback',0
cpu_top_amd8_text: db 'AMD leaf 80000008h',0
cpu_pae_text: db 'PAE HW ',0
cpu_xsave_text: db ' XSAVE HW ',0
cpu_osxsave_text: db ' OSXSAVE now ',0
cpu_avx_hw_text: db ' AVX HW ',0
cpu_avx_active_text: db ' AVX usable now ',0
