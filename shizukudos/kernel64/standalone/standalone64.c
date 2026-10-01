/* SPDX-License-Identifier: GPL-2.0-only
 * Kernel64 "standalone" services: the hypercall ABI (abi/shz_abi.h) served by the kernel itself so the
 * Long Mode kernel and its Win64 processes can run without the Supervisor, e.g. under QEMU TCG where Intel VMX
 * does not exist. This is a test/bring-up profile: it is NOT the multikernel product path and never claims
 * Supervisor or VMX behaviour.  Devices: kcommon/standalone_dev.h.
 *
 *   console   -> COM1            exit     -> "SHZ-EXIT:<code>" on COM1, then QEMU isa-debug-exit (0xF4)
 *   evidence  -> "SHZ-EV <slot> <hex>" on COM1, parsed by tests/run_k*_standalone.py
 *   timer     -> PIT ch.0 via the 8259 (IRQ0 = VEC_TIMER)     time -> timer ticks     walltime -> CMOS RTC
 *   doorbells/notify -> SHZ_E_UNSUPPORTED (single domain, no peers)
 */
#include "k64.h"
#include "boot_console.h"
#include "../../kcommon/standalone_dev.h"
#include "../../kcommon/boot_interrupts.h"

static int interrupt_route_ready;
static void boot_spurious_irq(struct regs *r) { (void)r; }

/* UEFI may hand over with LINT0 masked, a raised task priority or IMCR in
 * APIC mode. Restore the single-BSP virtual-wire path before enabling PIT.
 * This is the original implementation of the documented register protocol;
 * EDK2's ProgramVirtualWireMode is a useful primary reference:
 * https://github.com/tianocore/edk2/blob/master/UefiCpuPkg/Library/BaseXApicX2ApicLib/BaseXApicX2ApicLib.c
 */
static long standalone_timer_route(void)
{
    uint32_t a = 1, b, c, d, old, updated; uint64_t base;
    volatile uint32_t *apic = 0;
    uint8_t imcr;
    if (interrupt_route_ready) return SHZ_OK;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    (void)b; (void)c;
    /* A missing IMCR commonly reads FF; avoid writing a nonexistent register. */
    sa_outb(0x22, 0x70); imcr = sa_inb(0x23);
    if (imcr != 0xff) sa_outb(0x23, imcr & (uint8_t)~1u);
    if (!(d & (1u << 9))) { interrupt_route_ready = 1; return SHZ_OK; }
    base = rdmsr(0x1b);
    if (!(base & (1ull << 11))) { interrupt_route_ready = 1; return SHZ_OK; }
    irq_register(0xff, boot_spurious_irq);
    if (base & (1ull << 10)) {
        old = (uint32_t)rdmsr(0x835);
        updated = shz_boot_lint0_extint(old);
        wrmsr(0x808, 0);                         /* task priority */
        wrmsr(0x80f, (rdmsr(0x80f) & ~255ull) | 0x1ff);
        wrmsr(0x832, rdmsr(0x832) | (1u << 16)); /* stop firmware's timer */
        wrmsr(0x838, 0);
        wrmsr(0x835, updated);
    } else {
        const uint64_t va = UINT64_C(0xffffc00010000000);
        if (vm_map(kernel_pml4(), va, base & UINT64_C(0x000ffffffffff000), PT_W | PT_NX | PT_PCD | PT_PWT))
            return SHZ_E_RANGE;
        apic = (volatile uint32_t *)(uintptr_t)va;
        old = apic[0x350 / 4]; updated = shz_boot_lint0_extint(old);
        apic[0x80 / 4] = 0;
        apic[0xf0 / 4] = (apic[0xf0 / 4] & ~255u) | 0x1ff;
        apic[0x320 / 4] |= 1u << 16;
        apic[0x380 / 4] = 0;
        apic[0x350 / 4] = updated;
    }
    kprintf("K64: legacy timer route %s LINT0=%08x->%08x IMCR=%02x\n",
            base & (1ull << 10) ? "x2APIC" : "xAPIC", old, updated, imcr);
    interrupt_route_ready = 1;
    return SHZ_OK;
}

extern uint64_t arch_timer_irqs(void);
void standalone_eoi(void) { sa_eoi(); }
void standalone_eoi_irq(unsigned vector) { sa_eoi_irq(vector - sa_irq_vector(0)); }
void standalone_irq_unmask(unsigned irq) { sa_irq_unmask(irq); }
void standalone_irq_mask(unsigned irq) { sa_irq_mask(irq); }
unsigned standalone_irq_vector(unsigned irq) { return sa_irq_vector(irq); }

long shz_standalone_hcall(hcreg_t op, hcreg_t a, hcreg_t b, hcreg_t *value_out)
{
    hcreg_t v = 0;
    long st = SHZ_OK;
    switch (op) {
    case SHZ_HC_CONSOLE_WRITE: {
        const char *s = (const char *)p2v(a);
        uint64_t i;
        if (b > 512) { st = SHZ_E_RANGE; break; }
        k64_boot_console_write(s, (size_t)b);
        for (i = 0; i < b; ++i)
            sa_serial_putc(s[i]);
        break;
    }
    case SHZ_HC_EXIT:
        k64_boot_console_exit((unsigned)a);
        if (a <= 1) { extern void vfs_shutdown(void); vfs_shutdown(); }   /* normal exit: commit + flush write-back volumes */
        sa_exit((unsigned)a);
    case SHZ_HC_TIMER_SET:
        st = standalone_timer_route();
        if (!st) st = sa_timer_set((unsigned)a, (uint32_t)b);
        break;
    case SHZ_HC_WAIT: __asm__ volatile("sti; hlt"); break;
    case SHZ_HC_TIME: v = arch_timer_irqs() * TICK_US * 1000ull; break;
    case SHZ_HC_EVIDENCE:
        if (a > 31) { st = SHZ_E_RANGE; break; }
        sa_evidence((unsigned)a, b);
        break;
    case SHZ_HC_ABI_VERSION: v = ((hcreg_t)SHZ_ABI_MAJOR << 16) | SHZ_ABI_MINOR; break;
    case SHZ_HC_WALLTIME: v = sa_rtc_epoch(); break;
    case SHZ_HC_NOTIFY:
    case SHZ_HC_SET_DOORBELL_VECTOR:
    case SHZ_HC_DOORBELL_ACK:
    case SHZ_HC_DOMAIN_STATE:
    case SHZ_HC_CHANNEL_INFO:
        st = SHZ_E_UNSUPPORTED;
        break;
    default:
        st = SHZ_E_INVALID;
        break;
    }
    if (value_out)
        *value_out = v;
    return st;
}
