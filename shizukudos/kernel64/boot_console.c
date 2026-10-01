/* SPDX-License-Identifier: GPL-2.0-only */
#include "boot_console.h"
#include "../kcommon/boot_console.h"

static shz_boot_console_t console;
static int enabled = 1;
static int failure_visible;
static char stage[96] = "KERNEL STARTING", line[160], last[160];
static unsigned line_len;
static void copy_text(char *out, const char *s, unsigned cap)
{
    unsigned i = 0;
    while (s && s[i] && i + 1 < cap) { out[i] = s[i]; ++i; }
    out[i] = 0;
}
void *k64_boot_framebuffer_map(void)
{
    k64_boot_fb_t fb; uint64_t first, size, off, pa;
    if (console.pixels) return (void *)console.pixels;
    if (k64_boot_framebuffer(&fb) || !fb.pitch || fb.width > 16384 || fb.height > 16384) return 0;
    size = (uint64_t)fb.pitch * fb.height;
    first = fb.base & ~UINT64_C(4095); off = fb.base - first;
    if (size > 256ull * 1024 * 1024 || size + off > 256ull * 1024 * 1024) return 0;
    for (pa = 0; pa < size + off; pa += PAGE_SIZE)
        if (vm_map(kernel_pml4(), SHZ_BOOT_FB_VA + pa, first + pa, PT_W | PT_NX | PT_PCD | PT_PWT)) return 0;
    console.pixels = (volatile uint32_t *)(uintptr_t)(SHZ_BOOT_FB_VA + off);
    console.width = fb.width; console.height = fb.height;
    console.pitch_pixels = fb.pitch / 4; console.format = fb.format;
    return (void *)console.pixels;
}
void k64_boot_console_stage(const char *name, const char *detail)
{
    copy_text(stage, name, sizeof stage);
    if (enabled) shz_boot_console_draw(&console, stage, detail, 0x53f2c1);
}
void k64_boot_console_fail(const char *name, const char *detail)
{
    copy_text(stage, name, sizeof stage);
    failure_visible = 1;
    shz_boot_console_draw(&console, stage, detail, 0xff7878);
}
void k64_boot_console_enable(int on) { enabled = !!on; }
void k64_boot_console_write(const char *bytes, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        char ch = bytes[i];
        if (ch == '\n') {
            line[line_len] = 0; copy_text(last, line, sizeof last);
            if (!strncmp(line, "K64 PANIC:", 10)) k64_boot_console_fail("KERNEL PANIC", line);
            else if (enabled && !failure_visible) shz_boot_console_draw(&console, stage, line, 0x53f2c1);
            line_len = 0;
        } else if (ch != '\r' && line_len + 1 < sizeof line) line[line_len++] = ch;
    }
}
void k64_boot_console_exit(unsigned code)
{
    if (code && !failure_visible) k64_boot_console_fail("KERNEL STOPPED WITH AN ERROR", last);
    else if (code) return;
    else {
        enabled = 1;
        k64_boot_console_stage("SHUTDOWN REQUESTED", "ShizukuDOS has stopped. You may power off this machine.");
    }
}
