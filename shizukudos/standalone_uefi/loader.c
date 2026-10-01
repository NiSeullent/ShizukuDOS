/* SPDX-License-Identifier: GPL-2.0-only
 * Original IA32/x64 UEFI boot application. Both firmware architectures start
 * the same Kernel64. No CSM, VMX, imported binary or firmware call after EBS.
 * Payloads and page tables are staged in allocated memory; fixed legacy windows
 * are reclaimed only after EBS and validation of its final descriptor map.
 */
#include "../supervisor/loader/efi_ext.h"
#include "../uefi/boot.h"
#include "../abi/shz_abi.h"
#include "../kernel64/standalone/memholes.h"
#include "../kcommon/boot_console.h"
#include "config.h"

extern const uint8_t dos_kernel_start[], dos_kernel_end[];
extern const uint8_t dos_initrd_start[], dos_initrd_end[];
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);
EFI_STATUS (EFIAPI *volatile dos_entry_address)(EFI_HANDLE, EFI_SYSTEM_TABLE *) = efi_main;
#define KERNEL_PA UINT64_C(0x100000)
#define INITRD_PA UINT64_C(0x2000000)
#define RAM_MIN (64ull << 20)
#define RAM_MAX (256ull << 20)
#define MODES_PA UINT64_C(0x10000)
#define MODES_PAGES 96u
#define KERNEL_ENTRY UINT64_C(0xffffffff80100000)
#define LOW_BYTES 0x7000u
#define KERNEL_BYTES 0x200000u
#define STACK_BYTES 0x4000u
#define CONTROL_OFF 0x4000u
#define GDT_OFF 0x4800u
#define GDTR_OFF 0x4820u

static SD_HANDOFF handoff;
static shz_memplan_result_t plan;
static EFI_SYSTEM_TABLE *system_table;
static shz_boot_console_t boot_console;
static int uart_present;
static EFI_GUID gop_guid = {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};

/* All gateway operands are physical addresses below 256 MiB. This layout is
 * shared by its code32 and code64 entries; no C relocation enters the blob. */
typedef struct {
    uint32_t stack_top, low_stage, kernel_stage, initrd_stage;
    uint32_t initrd_bytes, page_tables, long_entry;
    uint16_t long_selector, reserved;
    uint32_t gdtr;
    uint8_t pad[28];
    uint8_t temporary_gdtr[10];
} gateway_control_t;
_Static_assert(offsetof(gateway_control_t, temporary_gdtr) == 64, "gateway GDTR offset");

static inline void out8(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t in8(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void serial_init(void)
{
    uint8_t saved = in8(0x3ff);
    out8(0x3ff, 0x5a);
    if (in8(0x3ff) != 0x5a) return;
    out8(0x3ff, 0xa5);
    if (in8(0x3ff) != 0xa5) { out8(0x3ff, saved); return; }
    out8(0x3ff, saved); uart_present = 1;
    out8(0x3f9, 0); out8(0x3fb, 0x80); out8(0x3f8, 1); out8(0x3f9, 0);
    out8(0x3fb, 3); out8(0x3fa, 0xc7); out8(0x3fc, 0x0b);
}
static void serial(const char *s)
{
    for (; *s; ++s) {
        unsigned spin;
        out8(0xe9, (uint8_t)*s);
        if (!uart_present) continue;
        for (spin = 0; spin < 4096 && !(in8(0x3fd) & 0x20); ++spin) ;
        if (spin == 4096) { uart_present = 0; continue; }
        out8(0x3f8, (uint8_t)*s);
    }
}
static void say(const char *s)
{
    CHAR16 text[160]; size_t n;
    serial(s);
    if (!system_table || !system_table->console_out || !system_table->console_out->output_string) return;
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
static char *append(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *hex(char *p, uint64_t n)
{
    unsigned i; p = append(p, "0x");
    for (i = 0; i < 16; ++i) *p++ = "0123456789ABCDEF"[(n >> (60 - 4 * i)) & 15];
    *p = 0; return p;
}
static void diagnostic(const char *stage, EFI_STATUS status, uint64_t address, uint64_t bytes, int firmware)
{
    char text[512], detail[128], *p;
    p = append(text, "DOS-UEFI: "); p = append(p, stage); p = append(p, " EFI_STATUS="); p = hex(p, status);
    p = append(p, " address="); p = hex(p, address); p = append(p, " bytes="); p = hex(p, bytes); append(p, "\n");
    if (firmware) say(text); else serial(text);
    p = append(detail, "status "); p = hex(p, status); p = append(p, "  address "); hex(p, address);
    shz_boot_console_draw(&boot_console, stage, detail, EFI_ERROR(status) ? 0xff6767 : 0x5de5c7);
}
static __attribute__((noreturn)) void stopped(const char *stage, EFI_STATUS status, uint64_t address, uint64_t bytes, int firmware)
{
    diagnostic(stage, status, address, bytes, firmware);
    if (firmware) say("64 MiB usable RAM minimum; 256 MiB recommended. Reset after correcting the error.\n");
    else serial("DOS-UEFI: FAILED; 64 MiB usable RAM minimum; 256 MiB recommended.\n");
    shz_boot_console_text(&boot_console, 20, 113, "64 MiB usable RAM minimum; 256 MiB recommended.", 0xffffff);
    for (;;) __asm__ volatile("cli; hlt");
}
static int standalone_kernel(size_t n)
{
    const char *needle = "SHZ-EXIT:"; size_t i, k;
    for (i = 0; i < n; ++i) {
        for (k = 0; needle[k] && i + k < n && dos_kernel_start[i + k] == (uint8_t)needle[k]; ++k) ;
        if (!needle[k]) return 1;
    }
    return 0;
}
static int usable(const EFI_MEMORY_DESCRIPTOR *d)
{
    return ((d->type >= EFI_LOADER_CODE_MEM && d->type <= EFI_BS_DATA) || d->type == EFI_CONVENTIONAL) &&
           (d->attributes & EFI_MEMORY_WB) && !(d->attributes & (UINT64_C(1) << 63));
}
static int memory_plan(const SD_HANDOFF *h, size_t initrd_size)
{
    static shz_memplan_t runs; size_t off;
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
    /* Some virtual GOP implementations place scanout in ordinary RAM. It must
     * survive independently of the descriptor type that firmware reports. */
    if (h->framebuffer.size)
        shz_memplan_remove(&runs, h->framebuffer.base, h->framebuffer.base + h->framebuffer.size);
    if (!shz_memplan_covers(&runs, MODES_PA, MODES_PA + MODES_PAGES * 4096))
        return shz_memplan_refuse(&plan, "Real/Protected Mode arena is not reclaimable RAM", MODES_PA);
    /* Unlike mere overlap with a hole, full coverage also rejects an initrd
     * destination outside every usable run. Reserved/runtime descriptors win. */
    if (!shz_memplan_covers(&runs, INITRD_PA, INITRD_PA + initrd_size))
        return shz_memplan_refuse(&plan, "DOS archive destination is not reclaimable RAM", INITRD_PA);
    return shz_memplan_solve(&runs, RAM_MAX, RAM_MIN, INITRD_PA, initrd_size, &plan);
}
static int overlaps(uint64_t a, uint64_t bytes, uint64_t b, uint64_t end)
{
    return a < end && a + bytes > b;
}
static EFI_STATUS safe_stage(EFI_BOOT_SERVICES *bs, size_t pages, uint32_t type, size_t initrd_size, uint64_t *address)
{
    EFI_ALLOCATE_PAGES_FN allocate = (EFI_ALLOCATE_PAGES_FN)bs->allocate_pages;
    EFI_FREE_PAGES_FN free_pages = (EFI_FREE_PAGES_FN)bs->free_pages;
    uint64_t cap = RAM_MAX - 1, bytes = (uint64_t)pages << 12; unsigned attempt;
    EFI_STATUS status;
    for (attempt = 0; attempt < 8; ++attempt) {
        *address = cap;
        status = allocate(EFI_ALLOCATE_MAX_ADDRESS, type, pages, address);
        if (EFI_ERROR(status)) return status;
        if (!overlaps(*address, bytes, 0, 0x80000) && !overlaps(*address, bytes, KERNEL_PA, 0x300000) &&
            !overlaps(*address, bytes, INITRD_PA, INITRD_PA + initrd_size) &&
            !overlaps(*address, bytes, handoff.framebuffer.base, handoff.framebuffer.base + handoff.framebuffer.size))
            return EFI_SUCCESS;
        free_pages(*address, pages);
        if (overlaps(*address, bytes, handoff.framebuffer.base, handoff.framebuffer.base + handoff.framebuffer.size)) {
            if (!handoff.framebuffer.base) return EFI_OUT_OF_RESOURCES;
            cap = handoff.framebuffer.base - 1;
        } else if (overlaps(*address, bytes, INITRD_PA, INITRD_PA + initrd_size)) cap = INITRD_PA - 1;
        else if (overlaps(*address, bytes, KERNEL_PA, 0x300000)) cap = KERNEL_PA - 1;
        else return EFI_OUT_OF_RESOURCES;
    }
    return EFI_OUT_OF_RESOURCES;
}
static uint64_t ticks(void)
{
    uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo;
}
static int cpu_supported(void)
{
    uint32_t a = 0x80000000u, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (a < 0x80000001u) return 0;
    a = 0x80000001u;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (d & ((1u << 29) | (1u << 20))) == ((1u << 29) | (1u << 20));
}
static int canonical_info(const EFI_GOP_INFO *in, EFI_GOP_INFO *out)
{
    if (!in || in->version || in->width < 640 || in->height < 480 ||
        in->width > 4096 || in->height > 4096 || in->pixels_per_scan_line < in->width ||
        in->pixels_per_scan_line > 16384) return 0;
    *out = *in;
    if (out->pixel_format <= 1) return 1;
    if (out->pixel_format != 2 || out->green_mask != 0x0000ff00u || out->reserved_mask != 0xff000000u) return 0;
    if (out->red_mask == 0x000000ffu && out->blue_mask == 0x00ff0000u) out->pixel_format = 0;
    else if (out->red_mask == 0x00ff0000u && out->blue_mask == 0x000000ffu) out->pixel_format = 1;
    else return 0;
    return 1;
}
static EFI_STATUS current_framebuffer(EFI_GOP *gop, SD_FRAMEBUFFER *fb)
{
    EFI_GOP_INFO info; EFI_GOP_MODE mode;
    if (!gop || !gop->mode || gop->mode->info_size < sizeof info || !canonical_info(gop->mode->info, &info))
        return EFI_UNSUPPORTED;
    mode = *gop->mode; mode.info = &info;
    return sd_framebuffer_snapshot(&mode, fb);
}
static EFI_STATUS discover_framebuffer(EFI_BOOT_SERVICES *bs, EFI_GOP *gop, SD_FRAMEBUFFER *fb)
{
    typedef EFI_STATUS (EFIAPI *QUERY)(EFI_GOP *, uint32_t, size_t *, EFI_GOP_INFO **);
    typedef EFI_STATUS (EFIAPI *SET)(EFI_GOP *, uint32_t);
    EFI_STATUS status = current_framebuffer(gop, fb); uint32_t index, count;
    if (!EFI_ERROR(status)) return status;
    if (!gop || !gop->mode || !gop->query_mode || !gop->set_mode) return status;
    count = gop->mode->max_mode; if (count > 128) count = 128;
    for (index = 0; index < count; ++index) {
        EFI_GOP_INFO *info = 0, canonical; size_t bytes = 0; int supported;
        status = ((QUERY)gop->query_mode)(gop, index, &bytes, &info);
        supported = !EFI_ERROR(status) && bytes >= sizeof canonical && canonical_info(info, &canonical);
        if (info) bs->free_pool(info);
        if (!supported) continue;
        status = ((SET)gop->set_mode)(gop, index);
        if (!EFI_ERROR(status) && !EFI_ERROR(status = current_framebuffer(gop, fb))) return status;
    }
    return EFI_UNSUPPORTED;
}

/* Executed from allocated EfiLoaderCode, never from LoaderData. On EFI32 the
 * new identity map is installed before reclaiming memory and the far jump
 * activates genuine long mode. The gateway has no references to EFI image data,
 * firmware stack, descriptor map or boot services once reclaim begins. */
#if UINTPTR_MAX == UINT32_MAX
#define GAS_SYMBOL(x) "_" x
__asm__(".text\n.globl _dos_gateway_start\n.globl _dos_gateway_long\n.globl _dos_gateway_end\n.p2align 4\n"
        ".code32\n_dos_gateway_start:\n"
        "cli\ncld\nmovl 4(%esp), %esi\nmovl 0(%esi), %esp\nmovl 32(%esi), %eax\nlgdt (%eax)\n"
        "movl %cr0, %eax\nandl $0x7fffffff, %eax\nmovl %eax, %cr0\n"
        "movl $0x20, %eax\nmovl %eax, %cr4\nmovl 20(%esi), %eax\nmovl %eax, %cr3\n"
        "movl $0xc0000080, %ecx\nrdmsr\norl $0x900, %eax\nwrmsr\n"
        "movl %cr0, %eax\norl $0x80000001, %eax\nandl $0xfffffff3, %eax\nmovl %eax, %cr0\n"
        "ljmp *24(%esi)\n.code64\n_dos_gateway_long:\nmovl %esi, %ebx\n");
#else
#define GAS_SYMBOL(x) x
__asm__(".text\n.globl dos_gateway_start\n.globl dos_gateway_long\n.globl dos_gateway_end\n.p2align 4\n"
        "dos_gateway_start:\ncli\ncld\nmovl %ecx, %ebx\nmovl 0(%rbx), %esp\n"
        "movl 20(%rbx), %eax\nmovq %rax, %cr3\nmovq $0x20, %rax\nmovq %rax, %cr4\nclts\n"
        "dos_gateway_long:\n");
#endif
__asm__("movl 0(%rbx), %esp\n"
        "movl 4(%rbx), %esi\nmovl $0x1000, %edi\nmovl $0x7000, %ecx\nrep movsb\n"
        "movl 8(%rbx), %esi\nmovl $0x100000, %edi\nmovl $0x200000, %ecx\nrep movsb\n"
        "movl 12(%rbx), %esi\nmovl $0x2000000, %edi\nmovl 16(%rbx), %ecx\nrep movsb\n"
        "xorl %eax, %eax\nmovl $0x10000, %edi\nmovl $0x60000, %ecx\nrep stosb\n"
        /* Relocate staged page-table links to their final low addresses. */
        "movq $0x2003, 0x1000\nmovq $0x4003, 0x1ff8\nmovq $0x3003, 0x2000\nmovq $0x3003, 0x4ff0\n"
        "movq $0x1000, %rax\nmovq %rax, %cr3\nlgdt 0x5820\n"
        "pushq $0x08\nleaq 1f(%rip), %rax\npushq %rax\nlretq\n"
        "1: movw $0x10, %ax\nmovw %ax, %ds\nmovw %ax, %es\nmovw %ax, %ss\n"
        "xorl %eax, %eax\nmovw %ax, %fs\nmovw %ax, %gs\n"
        "movq $0x7000, %rdi\nxorl %ebp, %ebp\nmovabsq $0xffffffff80100000, %rax\njmpq *%rax\n"
        GAS_SYMBOL("dos_gateway_end") ":\n");
/* Return the assembler to the file's native mode for any compiler output. */
#if UINTPTR_MAX == UINT32_MAX
__asm__(".code32\n");
#endif
extern const uint8_t dos_gateway_start[], dos_gateway_long[], dos_gateway_end[];

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES *bs; EFI_STALL_FN stall; EFI_STATUS status;
    uint64_t data_pa = 0, code_pa = 0, cr4, t0, t1;
    const size_t ksize = (size_t)(dos_kernel_end - dos_kernel_start);
    const size_t isize = (size_t)(dos_initrd_end - dos_initrd_start);
    const size_t gsize = (size_t)(dos_gateway_end - dos_gateway_start);
    const size_t initrd_pages = (isize + 4095) >> 12;
    const size_t stage_pages = (LOW_BYTES + KERNEL_BYTES + STACK_BYTES) / 4096 + initrd_pages;
    uint8_t *low; shz_bootinfo_t *bi; gateway_control_t *ctl;
    uint64_t *pml4, *pdpt_lo, *pd, *pdpt_hi, *gdt;
    EFI_GOP *gop = 0; SD_FRAMEBUFFER fb; size_t i;
    system_table = st; serial_init();
    if (!st || st->header.signature != EFI_SYSTEM_TABLE_SIGNATURE || !(bs = st->boot_services) ||
        bs->header.signature != EFI_BOOT_SERVICES_SIGNATURE) return EFI_INVALID_PARAMETER;
#if UINTPTR_MAX == UINT32_MAX
    say("ShizukuDOS 10 DOS-only: native EFI32 -> Kernel64 boot, no VMX required.\n");
    say("DOS-UEFI: firmware ABI=IA32\n");
    { uint32_t r; __asm__ volatile("mov %%cr4, %0" : "=r"(r)); cr4 = r; }
#else
    say("ShizukuDOS 10 DOS-only: native UEFI64 boot, no VMX required.\n");
    say("DOS-UEFI: firmware ABI=X64\n");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
#endif
    say("64 MiB usable RAM minimum; 256 MiB recommended.\n");
    if (bs->set_watchdog_timer) bs->set_watchdog_timer(0, 0, 0, 0);
    status = bs->locate_protocol(&gop_guid, 0, (void **)&gop);
    if (!EFI_ERROR(status) && gop) {
        status = discover_framebuffer(bs, gop, &fb);
        if (!EFI_ERROR(status) && fb.base <= UINTPTR_MAX && fb.size <= UINTPTR_MAX - (uintptr_t)fb.base) {
            boot_console = (shz_boot_console_t){(volatile uint32_t *)(uintptr_t)fb.base, fb.width, fb.height,
                fb.pitch_pixels, fb.pixel_format == 0 ? SHZ_FB_RGBX8888 : SHZ_FB_BGRX8888};
        }
    }
    if (!shz_boot_console_valid(&boot_console)) stopped("GOP framebuffer unavailable", status ? status : EFI_UNSUPPORTED, 0, 0, 1);
    handoff.framebuffer = fb;
    diagnostic("EFI entry and GOP ready", EFI_SUCCESS, fb.base, fb.size, 1);
    if (!cpu_supported()) stopped("Long Mode/NX required: use a 64-bit VM CPU", EFI_UNSUPPORTED, 0, 0, 1);
    if (cr4 & (1ull << 12)) stopped("Five-level EFI paging unsupported", EFI_UNSUPPORTED, cr4, 0, 1);
    if (ksize < 64 || ksize > 0x100000 || !standalone_kernel(ksize) || !isize || isize > (64u << 20) ||
        !gsize || gsize > 4096) stopped("Invalid embedded kernel/archive", EFI_LOAD_ERROR, ksize, isize, 1);
    status = safe_stage(bs, stage_pages, EFI_MEM_LOADER_DATA, isize, &data_pa);
    diagnostic("Allocate payload and owned stack", status, data_pa, (uint64_t)stage_pages << 12, 1);
    if (EFI_ERROR(status)) stopped("Payload allocation failed", status, data_pa, (uint64_t)stage_pages << 12, 1);
    status = safe_stage(bs, 1, EFI_MEM_LOADER_CODE, isize, &code_pa);
    diagnostic("Allocate executable gateway", status, code_pa, 4096, 1);
    if (EFI_ERROR(status)) stopped("Executable gateway allocation failed", status, code_pa, 4096, 1);
    low = (uint8_t *)(uintptr_t)data_pa;
    zero(low, stage_pages << 12);
    copy(low + LOW_BYTES, dos_kernel_start, ksize);
    copy(low + LOW_BYTES + KERNEL_BYTES, dos_initrd_start, isize);
    copy((void *)(uintptr_t)code_pa, dos_gateway_start, gsize);
    pml4 = (uint64_t *)low; pdpt_lo = (uint64_t *)(low + 0x1000);
    pd = (uint64_t *)(low + 0x2000); pdpt_hi = (uint64_t *)(low + 0x3000);
    pml4[0] = (data_pa + 0x1000) | 3; pml4[511] = (data_pa + 0x3000) | 3;
    pdpt_lo[0] = (data_pa + 0x2000) | 3; pdpt_hi[510] = (data_pa + 0x2000) | 3;
    /* Full 1-GiB identity aperture keeps all owned staging executable through the
     * transition even when usable RAM ends below a firmware allocation. Kernel
     * replaces these temporary tables with its firmware-aware mappings. */
    for (i = 0; i < 512; ++i) pd[i] = ((uint64_t)i << 21) | 0x83;
    gdt = (uint64_t *)(low + GDT_OFF);
    gdt[0] = 0; gdt[1] = 0x00af9b000000ffffull; gdt[2] = 0x00cf93000000ffffull;
    low[GDTR_OFF] = 23;
    for (i = 0; i < 8; ++i) low[GDTR_OFF + 2 + i] = (uint8_t)(UINT64_C(0x5800) >> (8 * i));
    ctl = (gateway_control_t *)(low + CONTROL_OFF);
    ctl->stack_top = (uint32_t)(data_pa + (stage_pages << 12) - 16);
    ctl->low_stage = (uint32_t)data_pa; ctl->page_tables = (uint32_t)data_pa;
    ctl->kernel_stage = (uint32_t)(data_pa + LOW_BYTES);
    ctl->initrd_stage = (uint32_t)(data_pa + LOW_BYTES + KERNEL_BYTES); ctl->initrd_bytes = (uint32_t)isize;
    ctl->long_entry = (uint32_t)(code_pa + (dos_gateway_long - dos_gateway_start)); ctl->long_selector = 8;
    ctl->gdtr = (uint32_t)(data_pa + CONTROL_OFF + offsetof(gateway_control_t, temporary_gdtr));
    ctl->temporary_gdtr[0] = 23;
    for (i = 0; i < 8; ++i) ctl->temporary_gdtr[2 + i] = (uint8_t)((data_pa + GDT_OFF) >> (8 * i));
    bi = (shz_bootinfo_t *)(low + SHZ_BOOTINFO_GPA - 0x1000);
    bi->magic = SHZ_BOOTINFO_MAGIC; bi->abi_major = SHZ_ABI_MAJOR; bi->abi_minor = SHZ_ABI_MINOR;
    bi->size = sizeof *bi; bi->domain_id = SHZ_DOM_KERNEL64; bi->generation = 1; bi->flags = SHZ_BIF_UEFI_DIRECT;
    bi->kernel_gpa = KERNEL_PA; bi->kernel_size = ksize; bi->initrd_gpa = INITRD_PA; bi->initrd_size = isize;
    bi->fb_base = fb.base; bi->fb_size = fb.size; bi->fb_width = fb.width; bi->fb_height = fb.height;
    bi->fb_pitch = fb.pitch_pixels * 4; bi->fb_bpp = 32; bi->fb_format = boot_console.format;
    for (i = 0; DOS_CMDLINE[i] && i < SHZ_CMDLINE_MAX - 1; ++i) bi->cmdline[i] = DOS_CMDLINE[i];
    bi->cmdline_size = (uint32_t)i;
    stall = (EFI_STALL_FN)bs->stall;
    if (stall) { t0 = ticks(); stall(50000); t1 = ticks(); bi->tsc_hz = (t1 - t0) * 20; }
    say("DOS-UEFI: embedded Kernel64 + DOS64 archive ready; exiting boot services.\n");
    shz_boot_console_draw(&boot_console, "ExitBootServices", "Kernel64 and applications staged; validating firmware memory.", 0x5de5c7);
    status = sd_exit_boot_services(bs, image, &handoff);
    __asm__ volatile("cli" ::: "memory");
    if (!handoff.boot_services_exited) stopped("ExitBootServices failed", status, 0, handoff.exit_calls, !handoff.exit_attempted);
    if (!memory_plan(&handoff, isize)) stopped(plan.why, EFI_OUT_OF_RESOURCES, plan.at, plan.heap_hole_bytes, 0);
    bi->ram_size = plan.ram;
    shz_memholes_write((volatile shz_memholes_t *)(low + SHZ_MEMHOLES_GPA - 0x1000), &plan);
    diagnostic("Validated post-EBS reclaim", EFI_SUCCESS, KERNEL_PA, KERNEL_BYTES, 0);
    serial("DOS-UEFI: owned executable gateway + validated reclaim ready\n");
    serial("DOS-UEFI: ExitBootServices PASS; entering native Kernel64.\n");
    ((void (EFIAPI *)(gateway_control_t *))(uintptr_t)code_pa)(ctl);
    stopped("Kernel returned", EFI_ABORTED, KERNEL_ENTRY, 0, 0);
}
