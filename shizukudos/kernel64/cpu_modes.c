/* SPDX-License-Identifier: GPL-2.0-only
 * Genuine native Real/Protected Mode compatibility demonstrations launched
 * from the Long Mode kernel. The transition island lives in pages allocated
 * by the UEFI loader before ExitBootServices. There is no software emulator.
 */
#include "fs.h"
#include "cpu_modes.h"
#include "cpu_modes_formats.h"

#define CONTROL_PA 0x18000u
#define REAL_PSP 0x20000u
#define REAL_BODY 0x20100u
#define REAL_SEG 0x2010u
#define PM_IMAGE 0x40000u
struct mode_control {
    uint32_t requested, exit_code, completed;
    uint16_t rm_cs, rm_ip, rm_ss, rm_sp, rm_ds, reserved;
    uint32_t pm_entry;
    uint32_t real_cr0, real_efer, real_cs;
    uint32_t pm_cr0, pm_efer, pm_cs;
    uint32_t reserved2, console_len, fault;
    char console[1024];
};
_Static_assert(__builtin_offsetof(struct mode_control, console) == 64, "bridge control ABI");
extern const uint8_t cpu_modes_bridge_start[], cpu_modes_bridge_end[];
extern const uint32_t cpu_modes_rm_int21_offset, cpu_modes_rm_exit_offset, cpu_modes_rm_fault_offset;
extern const uint32_t cpu_modes_pm_int80_offset, cpu_modes_pm_fault_offset;
static int ready;
static unsigned completed;
static int busy;
static struct mode_control *control(void) { return (struct mode_control *)p2v(CONTROL_PA); }
static int launch_failed(void) { __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); return -1; }

int cpu_modes_init(void)
{
    uint64_t pa;
    uint32_t i;
    size_t bytes = (size_t)(cpu_modes_bridge_end - cpu_modes_bridge_start);
    uint8_t *low;
    if (ready) return 0;
    if (!bytes || bytes >= 0x7000) return -1;
    for (pa = CPU_MODES_LOW_BASE; pa < CPU_MODES_LOW_BASE + CPU_MODES_LOW_BYTES; pa += PAGE_SIZE)
        if (vm_map(kernel_pml4(), pa, pa, PT_W)) return -1;
    low = (uint8_t *)p2v(CPU_MODES_LOW_BASE);
    memset(low, 0, CPU_MODES_LOW_BYTES);
    memcpy(low, cpu_modes_bridge_start, bytes);
    /* A separate Real Mode interrupt table preserves firmware IVT memory.
     * Default exceptions/unsupported software interrupts abort the sample. */
    for (i = 0; i < 256; ++i) {
        uint16_t *iv = (uint16_t *)p2v(CONTROL_PA + 0x800 + i * 4);
        uint8_t *gate = (uint8_t *)p2v(CONTROL_PA + 0xc00 + i * 8);
        uint32_t handler = CPU_MODES_LOW_BASE + cpu_modes_pm_fault_offset;
        iv[0] = (uint16_t)cpu_modes_rm_fault_offset; iv[1] = 0x1000;
        if (i == 0x20) iv[0] = (uint16_t)cpu_modes_rm_exit_offset;
        if (i == 0x21) iv[0] = (uint16_t)cpu_modes_rm_int21_offset;
        if (i == 0x80) handler = CPU_MODES_LOW_BASE + cpu_modes_pm_int80_offset;
        gate[0] = (uint8_t)handler; gate[1] = (uint8_t)(handler >> 8);
        gate[2] = 0x18; gate[3] = 0; gate[4] = 0; gate[5] = 0x8e;
        gate[6] = (uint8_t)(handler >> 16); gate[7] = (uint8_t)(handler >> 24);
    }
    ready = 1;
    kprintf("MODES: native transition island ready; DOS MZ16 + PE32, no emulator\n");
    return 0;
}

static int load_mz(const uint8_t *image, size_t bytes)
{
    cpu_mz_info mz;
    uint8_t *psp = (uint8_t *)p2v(REAL_PSP), *body = (uint8_t *)p2v(REAL_BODY);
    struct mode_control *c = control();
    unsigned i;
    if (cpu_mz_validate(image, bytes, &mz)) return -1;
    memset(psp, 0, 0x20000);
    psp[0] = 0xcd; psp[1] = 0x20;
    psp[2] = 0; psp[3] = 0x40;       /* top of allocated conventional arena */
    psp[0x80] = 0; psp[0x81] = 13;   /* empty command tail */
    memcpy(body, image + mz.header, mz.body);
    for (i = 0; i < mz.reloc_count; ++i) {
        const uint8_t *r = image + mz.reloc_table + i * 4;
        uint32_t at = (uint32_t)cpu_u16(r + 2) * 16 + cpu_u16(r);
        uint16_t value = (uint16_t)(cpu_u16(body + at) + REAL_SEG);
        body[at] = (uint8_t)value; body[at + 1] = (uint8_t)(value >> 8);
    }
    c->rm_cs = (uint16_t)(REAL_SEG + mz.cs); c->rm_ip = mz.ip;
    c->rm_ss = (uint16_t)(REAL_SEG + mz.ss); c->rm_sp = mz.sp; c->rm_ds = REAL_PSP >> 4;
    return 0;
}

static int load_pe(const uint8_t *image, size_t bytes)
{
    cpu_pe32_info pe;
    uint8_t *mapped = (uint8_t *)p2v(PM_IMAGE);
    unsigned i;
    if (cpu_pe32_validate(image, bytes, &pe)) return -1;
    memset(mapped, 0, 0x30000);
    memcpy(mapped, image, pe.headers);
    for (i = 0; i < pe.sections; ++i) {
        const uint8_t *s = image + pe.section_table + i * 40;
        memcpy(mapped + cpu_u32(s + 12), image + cpu_u32(s + 20), cpu_u32(s + 16));
    }
    control()->pm_entry = PM_IMAGE + pe.entry;
    return 0;
}

static int run_one(const char *name, unsigned bits)
{
    char path[96];
    const char *prefix = "\\SHZ\\MODES\\";
    unsigned i = 0, j = 0;
    uint64_t got = 0, saved_cr3, irq;
    uint8_t *image;
    fsnode_t *n;
    struct mode_control *c;
    int failed, result;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return -1;
    if (cpu_modes_init()) return launch_failed();
    while (prefix[i]) { path[i] = prefix[i]; ++i; }
    while (name[j] && i + 1 < sizeof path) path[i++] = name[j++];
    path[i] = 0;
    n = fs_lookup(path);
    if (!n || n->is_dir || n->size < 32 || n->size > 0x10000) return launch_failed();
    image = kmalloc(n->size);
    if (!image) return launch_failed();
    if (fs_read(n, 0, image, n->size, &got) || got != n->size) { kfree(image); return launch_failed(); }
    c = control();
    memset(c, 0, sizeof *c);
    c->requested = bits;
    result = bits == 16 ? load_mz(image, n->size) : load_pe(image, n->size);
    kfree(image);
    if (result) { kprintf("MODES: %s invalid executable refused\n", name); return launch_failed(); }
    kprintf("MODES: starting %s in native %u-bit CPU mode\n", name, bits);
    irq = irq_save();
    saved_cr3 = read_cr3();
    write_cr3(kernel_pml4());
    ((void (*)(void))(uintptr_t)CPU_MODES_LOW_BASE)();
    write_cr3(saved_cr3);
    irq_restore(irq);
    c->console[c->console_len < 1024 ? c->console_len : 1023] = 0;
    kprintf("%s", c->console);
    failed = c->completed != bits || c->exit_code || c->fault;
    if (bits == 16) {
        failed |= (c->real_cr0 & 0x80000001u) != 0 || (c->real_efer & 0x500u) != 0;
        kprintf("MODES: REAL16 CR0=%08x EFER=%08x CS=%04x %s\n", c->real_cr0,
                c->real_efer, c->real_cs, failed ? "FAIL" : "PASS");
    } else {
        failed |= (c->pm_cr0 & 0x80000001u) != 1 || (c->pm_efer & 0x500u) != 0 || c->pm_cs != 0x18;
        kprintf("MODES: PROTECTED32 CR0=%08x EFER=%08x CS=%04x %s\n", c->pm_cr0,
                c->pm_efer, c->pm_cs, failed ? "FAIL" : "PASS");
    }
    failed |= (read_cr0() & 0x80000001ull) != 0x80000001ull || (rdmsr(MSR_EFER) & 0x500ull) != 0x500ull;
    kprintf("MODES: LONG64 CR0=%08x EFER=%08x return %s\n", (uint32_t)read_cr0(),
            (uint32_t)rdmsr(MSR_EFER), failed ? "FAIL" : "PASS");
    kprintf("MODES: %s exit=%08x fault=%u %s\n", name, c->exit_code, c->fault, failed ? "FAIL" : "PASS");
    if (!failed) ++completed;
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    return failed ? -1 : 0;
}

int cpu_modes_launch_named(const char *name)
{
    static const char *const real[] = {"HELLO.EXE", "MODE.EXE", "COUNT.EXE"};
    static const char *const pm[] = {"HELLO32.EXE", "MODE32.EXE", "COUNT32.EXE"};
    unsigned i;
    if (!name) return -1;
    for (i = 0; i < 3; ++i) {
        if (!strcmp(name, real[i])) return run_one(real[i], 16);
        if (!strcmp(name, pm[i])) return run_one(pm[i], 32);
    }
    return -1;
}
unsigned cpu_modes_completed(void) { return completed; }
unsigned cpu_modes_run_samples(void)
{
    const char *const names[] = {"HELLO.EXE", "MODE.EXE", "COUNT.EXE", "HELLO32.EXE", "MODE32.EXE", "COUNT32.EXE"};
    unsigned i, failures = 0;
    for (i = 0; i < sizeof names / sizeof names[0]; ++i) failures += cpu_modes_launch_named(names[i]) != 0;
    kprintf("MODES: completed 6 native application(s), %u failure(s)\n", failures);
    return failures;
}
