/* SPDX-License-Identifier: GPL-2.0-only
 * Original DOS-only UEFI application. The memory plan and transition follow the
 * project's direct Kernel64 boot contract; no Supervisor, CSM or VMX is used.
 * Kernel and native SD64 archive are embedded, so optical-media firmware only
 * needs to read this one EFI application from the El Torito FAT image.
 */
#include "../supervisor/loader/efi_ext.h"
#include "../uefi/boot.h"
#include "../abi/shz_abi.h"
#include "../kernel64/standalone/memholes.h"
#include "config.h"

extern const uint8_t dos_kernel_start[], dos_kernel_end[];
extern const uint8_t dos_initrd_start[], dos_initrd_end[];
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);
/* Ensure PE/COFF has a relocation section for firmware-selected load addresses. */
EFI_STATUS (EFIAPI *volatile dos_entry_address)(EFI_HANDLE, EFI_SYSTEM_TABLE *) = efi_main;

#define KERNEL_PA UINT64_C(0x100000)
#define INITRD_PA UINT64_C(0x2000000)
#define RAM_MIN (64ull << 20)
#define RAM_MAX (256ull << 20)
#define TRAMP_PA UINT64_C(0x5000)
#define GDT_PA UINT64_C(0x5800)
#define GDTR_PA UINT64_C(0x5820)
#define MODES_PA UINT64_C(0x10000)
#define MODES_PAGES 96u
#define KERNEL_ENTRY UINT64_C(0xffffffff80100000)

static SD_HANDOFF handoff;
static shz_memplan_result_t plan;
static EFI_SYSTEM_TABLE *system_table;
static EFI_GUID gop_guid = {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};

static inline void out8(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t in8(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void serial_init(void)
{
    out8(0x3f9, 0); out8(0x3fb, 0x80); out8(0x3f8, 1); out8(0x3f9, 0);
    out8(0x3fb, 3); out8(0x3fa, 0xc7); out8(0x3fc, 0x0b);
}
static void serial(const char *s)
{
    for (; *s; ++s) {
        unsigned spin;
        for (spin = 0; spin < 100000 && !(in8(0x3fd) & 0x20); ++spin) ;
        out8(0x3f8, (uint8_t)*s);
    }
}
static void say(const char *s)
{
    CHAR16 text[160];
    size_t n;
    serial(s);
    if (!system_table->console_out || !system_table->console_out->output_string) return;
    while (*s) {
        for (n = 0; n < 158 && *s; ++n) {
            if (*s == '\n') text[n++] = '\r';
            text[n] = (uint8_t)*s++;
        }
        text[n] = 0;
        system_table->console_out->output_string(system_table->console_out, text);
    }
}
static void zero(void *p, size_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void copy(void *p, const void *q, size_t n) { uint8_t *b = p; const uint8_t *a = q; while (n--) *b++ = *a++; }
static int standalone_kernel(size_t n)
{
    const char *needle = "SHZ-EXIT:";
    size_t i, k;
    for (i = 0; i < n; ++i) {
        for (k = 0; needle[k] && i + k < n && dos_kernel_start[i + k] == (uint8_t)needle[k]; ++k) ;
        if (!needle[k]) return 1;
    }
    return 0;
}
static int usable(const EFI_MEMORY_DESCRIPTOR *d)
{
    return d->type >= EFI_LOADER_CODE_MEM && d->type <= EFI_BS_DATA ? (d->attributes & EFI_MEMORY_WB) != 0 :
           d->type == EFI_CONVENTIONAL && (d->attributes & EFI_MEMORY_WB);
}
static int memory_plan(const SD_HANDOFF *h, uint64_t cap, size_t initrd_size)
{
    static shz_memplan_t runs;
    size_t off;
    shz_memplan_init(&runs);
    for (off = 0; off < h->map_size; off += h->descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)h->memory_map + off);
        uint64_t end = d->physical_start + (d->pages << 12);
        if (usable(d) && d->physical_start < (1ull << 32))
            shz_memplan_add(&runs, d->physical_start, end < (1ull << 32) ? end : 1ull << 32);
    }
    for (off = 0; off < h->map_size; off += h->descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)h->memory_map + off);
        if (!usable(d)) shz_memplan_remove(&runs, d->physical_start, d->physical_start + (d->pages << 12));
    }
    return shz_memplan_solve(&runs, cap, RAM_MIN, INITRD_PA, initrd_size, &plan);
}
static __attribute__((noreturn)) void halt(const char *why)
{
    serial("DOS-UEFI: FAILED: "); serial(why); serial("\n");
    for (;;) __asm__ volatile("cli; hlt");
}
static void release_pages(EFI_BOOT_SERVICES *bs, int low, int modes, int kernel, int initrd, size_t isize)
{
    EFI_FREE_PAGES_FN free_pages = (EFI_FREE_PAGES_FN)bs->free_pages;
    if (initrd) free_pages(INITRD_PA, (isize + 4095) >> 12);
    if (kernel) free_pages(KERNEL_PA, 512);
    if (modes) free_pages(MODES_PA, MODES_PAGES);
    if (low) free_pages(0x1000, 7);
}
/* Copied to a reserved identity-mapped low page before replacing firmware CR3.
 * Microsoft x64 ABI: RCX PML4, RDX GDTR, R8 bootinfo, R9 kernel entry. */
__asm__(".text\n.globl dos_tramp_start\n.globl dos_tramp_end\n.p2align 4\n"
        "dos_tramp_start:\n"
        "movq $0x7000, %rsp\nmovq %rcx, %cr3\nmovq $0x20, %rax\nmovq %rax, %cr4\nclts\nlgdt (%rdx)\n"
        "pushq $0x08\nleaq 1f(%rip), %rax\npushq %rax\nlretq\n"
        "1: movw $0x10, %ax\nmovw %ax, %ds\nmovw %ax, %es\nmovw %ax, %ss\n"
        "xorl %eax, %eax\nmovw %ax, %fs\nmovw %ax, %gs\nmovq %r8, %rdi\nxorl %ebp, %ebp\njmpq *%r9\n"
        "dos_tramp_end:\n");
extern const uint8_t dos_tramp_start[], dos_tramp_end[];

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES *bs;
    EFI_ALLOCATE_PAGES_FN allocate;
    EFI_STALL_FN stall;
    EFI_STATUS status;
    shz_bootinfo_t *bi = (shz_bootinfo_t *)(uintptr_t)SHZ_BOOTINFO_GPA;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)0x1000, *pdpt_lo = (uint64_t *)(uintptr_t)0x2000;
    uint64_t *pd = (uint64_t *)(uintptr_t)0x3000, *pdpt_hi = (uint64_t *)(uintptr_t)0x4000;
    uint64_t *gdt = (uint64_t *)(uintptr_t)GDT_PA, cr4, addr, t0, t1;
    uint8_t *gdtr = (uint8_t *)(uintptr_t)GDTR_PA;
    const size_t ksize = (size_t)(dos_kernel_end - dos_kernel_start);
    const size_t isize = (size_t)(dos_initrd_end - dos_initrd_start);
    const size_t tsize = (size_t)(dos_tramp_end - dos_tramp_start);
    EFI_GOP *gop = 0;
    SD_FRAMEBUFFER fb;
    size_t i;
    int low_alloc = 0, modes_alloc = 0, kernel_alloc = 0, initrd_alloc = 0;
    system_table = st;
    serial_init();
    if (!st || st->header.signature != EFI_SYSTEM_TABLE_SIGNATURE || !(bs = st->boot_services) ||
        bs->header.signature != EFI_BOOT_SERVICES_SIGNATURE) return EFI_INVALID_PARAMETER;
    say("ShizukuDOS 10 DOS-only: native UEFI64 boot, no VMX required.\n");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if ((cr4 & (1ull << 12)) || ksize < 64 || ksize > 0x100000 || !standalone_kernel(ksize) ||
        !isize || isize > (64u << 20) || !tsize || tsize > GDT_PA - TRAMP_PA) {
        say("DOS-UEFI: unsupported paging or invalid embedded kernel/archive.\n");
        return EFI_UNSUPPORTED;
    }
    if (bs->set_watchdog_timer) bs->set_watchdog_timer(0, 0, 0, 0);
    allocate = (EFI_ALLOCATE_PAGES_FN)bs->allocate_pages;
    addr = 0x1000;
    status = allocate(EFI_ALLOCATE_ADDRESS, EFI_MEM_LOADER_DATA, 7, &addr);
    low_alloc = !EFI_ERROR(status);
    if (!EFI_ERROR(status)) {
        /* The native Real/Protected Mode transition island and executable
         * arenas must belong to us before firmware boot services disappear. */
        addr = MODES_PA;
        status = allocate(EFI_ALLOCATE_ADDRESS, EFI_MEM_LOADER_DATA, MODES_PAGES, &addr);
        modes_alloc = !EFI_ERROR(status);
    }
    if (!EFI_ERROR(status)) {
        addr = KERNEL_PA;
        status = allocate(EFI_ALLOCATE_ADDRESS, EFI_MEM_LOADER_DATA, 512, &addr);
        kernel_alloc = !EFI_ERROR(status);
    }
    if (!EFI_ERROR(status)) {
        addr = INITRD_PA;
        status = allocate(EFI_ALLOCATE_ADDRESS, EFI_MEM_LOADER_DATA, (isize + 4095) >> 12, &addr);
        initrd_alloc = !EFI_ERROR(status);
    }
    if (EFI_ERROR(status)) {
        say("DOS-UEFI: firmware owns a required fixed address; kernel was not started.\n");
        release_pages(bs, low_alloc, modes_alloc, kernel_alloc, initrd_alloc, isize);
        return status;
    }
    zero((void *)(uintptr_t)0x1000, 0x7000);
    zero((void *)(uintptr_t)MODES_PA, MODES_PAGES * 4096);
    zero((void *)(uintptr_t)KERNEL_PA, 0x200000);
    copy((void *)(uintptr_t)KERNEL_PA, dos_kernel_start, ksize);
    copy((void *)(uintptr_t)INITRD_PA, dos_initrd_start, isize);
    copy((void *)(uintptr_t)TRAMP_PA, dos_tramp_start, tsize);
    gdt[0] = 0; gdt[1] = 0x00af9b000000ffffull; gdt[2] = 0x00cf93000000ffffull;
    gdtr[0] = 23; gdtr[1] = 0;
    for (i = 0; i < 8; ++i) gdtr[2 + i] = (uint8_t)(GDT_PA >> (8 * i));
    bi->magic = SHZ_BOOTINFO_MAGIC; bi->abi_major = SHZ_ABI_MAJOR; bi->abi_minor = SHZ_ABI_MINOR;
    bi->size = sizeof *bi; bi->domain_id = SHZ_DOM_KERNEL64; bi->generation = 1; bi->flags = SHZ_BIF_UEFI_DIRECT;
    bi->kernel_gpa = KERNEL_PA; bi->kernel_size = ksize; bi->initrd_gpa = INITRD_PA; bi->initrd_size = isize;
    for (i = 0; DOS_CMDLINE[i] && i < SHZ_CMDLINE_MAX - 1; ++i) bi->cmdline[i] = DOS_CMDLINE[i];
    bi->cmdline_size = (uint32_t)i;
    if (!EFI_ERROR(bs->locate_protocol(&gop_guid, 0, (void **)&gop)) && gop &&
        !EFI_ERROR(sd_framebuffer_snapshot(gop->mode, &fb))) {
        bi->fb_base = fb.base; bi->fb_size = fb.size; bi->fb_width = fb.width; bi->fb_height = fb.height;
        bi->fb_pitch = fb.pitch_pixels * 4; bi->fb_bpp = 32;
        bi->fb_format = fb.pixel_format == 0 ? SHZ_FB_RGBX8888 : SHZ_FB_BGRX8888;
    }
    stall = (EFI_STALL_FN)bs->stall;
    __asm__ volatile("rdtsc" : "=a"(addr), "=d"(t0)); t0 = (t0 << 32) | (uint32_t)addr;
    stall(50000);
    __asm__ volatile("rdtsc" : "=a"(addr), "=d"(t1)); t1 = (t1 << 32) | (uint32_t)addr;
    bi->tsc_hz = (t1 - t0) * 20;
    say("DOS-UEFI: embedded Kernel64 + DOS64 archive ready; exiting boot services.\n");
    status = sd_exit_boot_services(bs, image, &handoff);
    if (EFI_ERROR(status) && !handoff.exit_attempted) {
        if (handoff.memory_map) bs->free_pool(handoff.memory_map);
        release_pages(bs, low_alloc, modes_alloc, kernel_alloc, initrd_alloc, isize);
        return status;
    }
    __asm__ volatile("cli" ::: "memory");
    if (!handoff.boot_services_exited) halt("ExitBootServices failed");
    if (!memory_plan(&handoff, RAM_MAX, isize)) halt(plan.why);
    bi->ram_size = plan.ram;
    shz_memholes_write((volatile shz_memholes_t *)(uintptr_t)SHZ_MEMHOLES_GPA, &plan);
    pml4[0] = 0x2000 | 3; pml4[511] = 0x4000 | 3; pdpt_lo[0] = 0x3000 | 3; pdpt_hi[510] = 0x3000 | 3;
    for (i = 0; i < 512 && ((uint64_t)i << 21) < plan.ram; ++i) pd[i] = ((uint64_t)i << 21) | 0x83;
    serial("DOS-UEFI: ExitBootServices PASS; entering native Kernel64.\n");
    ((void (EFIAPI *)(uint64_t, uint64_t, uint64_t, uint64_t))(uintptr_t)TRAMP_PA)(0x1000, GDTR_PA, SHZ_BOOTINFO_GPA, KERNEL_ENTRY);
    halt("kernel returned");
}
