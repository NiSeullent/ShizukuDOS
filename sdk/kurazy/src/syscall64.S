/* SPDX-License-Identifier: GPL-2.0-only
 * Microsoft x64 C ABI -> Shizuku four-register syscall ABI. */
.text
.globl kurazy_call
kurazy_call:
    movq %rcx, %rax
    movq %rdx, %r10
    movq %r8, %rdx
    movq %r9, %r8
    movq 40(%rsp), %r9
    syscall
    ret
