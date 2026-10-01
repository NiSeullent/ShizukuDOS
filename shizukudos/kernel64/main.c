/* SPDX-License-Identifier: GPL-2.0-only
 * Kernel64 entry (C): CPU tables, memory, scheduler, timer, self-tests.
 */
#include "proc_internal.h"
#include "fs.h"
#include "dos64.h"
#include "cpu_modes.h"
#include "shizukugui.h"
#include "boot_console.h"

static shz_bootinfo_t bootinfo;
static unsigned desktop_failures;
int initrd_files = -1;                          /* -1: none or rejected; read by the Win64 self-test */

/* F8 runs the same full native/mode/media acceptance suite on the delivered
 * desktop ISO. Startup reaches a visible, interactive desktop first. */
unsigned shizukudos_acceptance_run(void)
{
    unsigned failures = cpu_modes_run_samples();
    failures += dos64_run_samples();
    failures += shizukugui_selftest() != 0;
    desktop_failures += failures;
    kprintf("SHZ-NATIVE-ACCEPT: %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}

static uint64_t boot_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* No sleep/hlt before IRQ0 has proved that it can wake the scheduler. TSC and
 * an independent iteration ceiling keep absent-PIT diagnostics bounded. */
static int timer_heartbeat(void)
{
    const uint64_t start = boot_tsc(), tick = ticks_now();
    uint64_t budget = bootinfo.tsc_hz;
    unsigned spins = 0;
    if (budget < 1000000 || budget > 20000000000ull) budget = 1000000000;
    while (ticks_now() - tick < 2 && boot_tsc() - start < budget && ++spins < 10000000u)
        __asm__ volatile("pause");
    return ticks_now() - tick >= 2 ? 0 : -1;
}

int k64_boot_framebuffer(k64_boot_fb_t *out)
{
    const shz_bootinfo_t *b = &bootinfo;
    if (!SHZ_BOOTINFO_HAS(b, fb_bpp) || !b->fb_base || b->fb_bpp != 32 ||
        (b->fb_format != SHZ_FB_RGBX8888 && b->fb_format != SHZ_FB_BGRX8888) || !b->fb_width || !b->fb_height ||
        b->fb_pitch / 4 < b->fb_width || (uint64_t)b->fb_pitch * b->fb_height > b->fb_size)
        return -1;
    out->base = b->fb_base;
    out->size = b->fb_size;
    out->width = b->fb_width;
    out->height = b->fb_height;
    out->pitch = b->fb_pitch;
    out->bpp = b->fb_bpp;
    out->format = b->fb_format;
    return 0;
}

const char *k64_boot_cmdline(void) { return bootinfo.cmdline; }

void kmain(uint64_t bootinfo_pa)
{
    /* The boot mapping still shows physical memory at the kernel alias. */
    const shz_bootinfo_t *bi = (const shz_bootinfo_t *)(K64_VIRT_BASE + bootinfo_pa);
    k64_boot_fb_t fb;
    if (bi->magic != SHZ_BOOTINFO_MAGIC || bi->abi_major != SHZ_ABI_MAJOR || bi->domain_id != SHZ_DOM_KERNEL64 ||
        bi->size < __builtin_offsetof(shz_bootinfo_t, fb_base))
        shz_exit(97);
    /* Copy what the writer provided (`size`); a 1.0 writer's missing tail stays zero. */
    memcpy(&bootinfo, bi, bi->size < sizeof bootinfo ? bi->size : sizeof bootinfo);
    bootinfo.size = bi->size < sizeof bootinfo ? bi->size : sizeof bootinfo;
    bootinfo.cmdline[SHZ_CMDLINE_MAX - 1] = 0;
    arch_init();
    mem_init(&bootinfo);
    (void)k64_boot_framebuffer_map();
    k64_boot_console_stage("KERNEL MEMORY READY", "Loading the native desktop and packaged files.");
    krandom_init(&bootinfo, sizeof bootinfo);       /* before anything that needs random bytes (ASLR, user RNG) */
    kprintf("%s: Long Mode kernel starting, %u MiB RAM, rip above 4 GiB, tsc %u kHz\n", KVER,
            (uint32_t)(bootinfo.ram_size >> 20), (uint32_t)(bootinfo.tsc_hz / 1000));
    kprintf("%s: boot info ABI %u.%u, %u bytes%s\n", KVER, bootinfo.abi_major, bootinfo.abi_minor, bootinfo.size,
            (bootinfo.flags & SHZ_BIF_UEFI_DIRECT) ? ", started directly by the UEFI boot manager (no Supervisor)" : "");
    if (bootinfo.cmdline[0])
        kprintf("%s: command line \"%s\"\n", KVER, bootinfo.cmdline);
    if (!k64_boot_framebuffer(&fb))
        kprintf("%s: UEFI GOP framebuffer %ux%u, pitch %u, %s, at %llx (%llu KiB): available through "
                "k64_boot_framebuffer(); ShizukuGUI uses the GOP framebuffer directly\n", KVER, fb.width, fb.height, fb.pitch,
                fb.format == SHZ_FB_BGRX8888 ? "BGRX" : "RGBX", fb.base, fb.size >> 10);
#ifdef SHZ_STANDALONE
    { extern void pci_log_devices(void); pci_log_devices(); }        /* device inventory; port I/O is only safe without the Supervisor */
#endif
    fs_init();
    if (bootinfo.initrd_size) {                 /* WIN64.IMG: \SHZ\SYS64 (ntdll, kernel32) and \SHZ\TESTS */
        const int files = fs_load_archive((const uint8_t *)p2v(bootinfo.initrd_gpa), bootinfo.initrd_size);
        if (files < 0)
            kprintf("%s: initrd archive rejected\n", KVER);
        else
            kprintf("%s: initrd mounted, %d file(s)\n", KVER, files);
        initrd_files = files;
    }
    /* The live desktop uses its immutable RAM archive. Optional AHCI probing
     * is explicitly selected; other diagnostic profiles retain their disks. */
    if (!dos64_boot_requested() || !k64_cmdline_has("shz.gui") || k64_cmdline_has("shz.disk")) {
        k64_boot_console_stage("DISK DISCOVERY", "Discovering optional disk volumes.");
        { extern void disk_init(void); disk_init(); }
    }
    k64_boot_console_stage("SCHEDULER STARTING", "Checking the PIT interrupt before any blocking wait.");
    sched_init();
    KASSERT(shz_timer_set(VEC_TIMER, TICK_US) == 0);
    sti();
    if (timer_heartbeat()) {
        k64_boot_console_fail("TIMER INTERRUPT UNAVAILABLE", "IRQ0 did not arrive. Check the VM's legacy PIC/PIT support.");
        kprintf("K64: timer heartbeat FAIL; no scheduler waits attempted\n");
        shz_exit(96);
    }
    kprintf("K64: timer heartbeat PASS\n");
    if (k64_cmdline_has("shz.modes"))
        shz_exit(cpu_modes_run_samples() ? 1 : 0);
    if (dos64_boot_requested()) {
        unsigned failures = 0;
        /* Headless mode still runs native tests automatically. The GUI's F8
         * action runs the full suite once a visible desktop is available. */
        if (k64_cmdline_has("shz.gui")) {
            k64_boot_console_stage("SHIZUKUGUI STARTING", "Preparing the GOP desktop, browser and media workers.");
            if (shizukugui_init()) {
                k64_boot_console_fail("SHIZUKUGUI INITIALIZATION FAILED", "The framebuffer, packaged media or worker allocation failed.");
                kprintf("SHZGUI: GOP initialization failed\n");
                shz_exit(1);
            }
            k64_boot_console_enable(0);
            if (k64_cmdline_has("shz.acceptance")) (void)shizukudos_acceptance_run();
            shizukugui_run();
            failures += desktop_failures;
        } else {
            failures += dos64_run_samples();
        }
        shz_exit(failures ? 1 : 0);
    }
    run_self_tests(&bootinfo);
    /* NT driver host: the single init call. A complete no-op unless the initrd carries
     * \SHZ\DRIVERS (only tests/run_k64_ntdrv.py mounts such an image), so default runs are
     * unaffected. See docs/shizukudos10/NTDRV.md and kernel64/ntdrv_*.c. */
    { extern void ntdrv_selftest(void); ntdrv_selftest(); }
    setup_autostart(&bootinfo);
    { extern void k64_autorun(void); k64_autorun(); }   /* shz.autorun=<control file>: one Win64 program (autorun.c) */
    if (bootinfo.channel_count) {
        ipc64_init(&bootinfo);
        if (ipc64_run_tests())
            kprintf("K64: IPC tests reported failures\n");
    }
    subsys64_start(&bootinfo);                  /* WIN64 subsystem bridge: serves a Win98 peer, or its loopback self-test when standalone */
    report_final();
    kprintf("%s: done, %u self-test failure(s)\n", KVER, tests_failed());
    shz_exit(tests_failed() ? 1 : 0);
}
