/* SPDX-License-Identifier: GPL-2.0-only
 * Host regression tests for the actual allocation-free renderer and UART
 * transport. No guest, port I/O, firmware or external library is required.
 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SHZ_STANDALONE_IO_TEST 1
#include "../kcommon/standalone_dev.h"
#include "../kcommon/boot_console.h"
#include "../kcommon/boot_interrupts.h"

static unsigned mode, reads, writes, e9_bytes, data_bytes;
static uint8_t lcr;
void sa_outb(uint16_t p, uint8_t v)
{
    ++writes;
    if (p == 0xe9) ++e9_bytes;
    if (p == SA_COM1 && lcr != 0x80) ++data_bytes;
    if (p == SA_COM1 + 3 && mode != 1) lcr = v;
}
uint8_t sa_inb(uint16_t p)
{
    ++reads;
    if (mode == 1) return 0xff;             /* no ISA UART */
    if (p == SA_COM1 + 3) return lcr;
    if (p == SA_COM1 + 5) return mode == 2 ? 0 : 0x20;
    return 0;
}
static void test_uart(unsigned profile)
{
    unsigned first_reads;
    mode = profile; reads = writes = e9_bytes = data_bytes = 0;
    lcr = 0; sa_serial_ready = 0;
    sa_serial_putc('A'); first_reads = reads;
    sa_serial_puts("BCDEFGHIJKLMNOPQRSTUVWXYZ");
    assert(e9_bytes == 26);
    if (profile == 0) { assert(data_bytes == 26 && sa_serial_ready == 1); }
    else {
        assert(data_bytes == 0 && sa_serial_ready == -1);
        assert(reads == first_reads);      /* no repeated absent/stalled probe */
        assert(reads <= SA_SERIAL_POLL_LIMIT + 2);
    }
}
static void test_framebuffer(unsigned format, unsigned width, unsigned height)
{
    const unsigned pitch = width + 32;
    const size_t n = (size_t)pitch * height;
    uint32_t *storage = malloc((n + 2) * sizeof *storage);
    shz_boot_console_t console;
    unsigned x, y, changed = 0;
    assert(storage);
    for (size_t i = 0; i < n + 2; ++i) storage[i] = 0xdeadbeef;
    console = (shz_boot_console_t){ storage + 1, width, height, pitch, format };
    shz_boot_console_draw(&console, "UEFI MEMORY READY", "Bounded GOP boot diagnostics", 0x123456);
    assert(storage[0] == 0xdeadbeef && storage[n + 1] == 0xdeadbeef);
    assert(storage[1] == shz_boot_console_pixel(0x123456, format));
    for (y = 0; y < height; ++y)
        for (x = 0; x < pitch; ++x) {
            uint32_t value = console.pixels[(size_t)y * pitch + x];
            if (x >= width || y >= 192) assert(value == 0xdeadbeef);
            else if (value != 0xdeadbeef) ++changed;
        }
    assert(changed == width * (height < 192 ? height : 192));
    shz_boot_console_text(&console, UINT_MAX, UINT_MAX, "overflow clipping", 0xff0000);
    shz_boot_console_rect(&console, width - 1, height - 1, UINT_MAX, UINT_MAX, 0xabcdef);
    assert(console.pixels[(size_t)(height - 1) * pitch + width - 1] == shz_boot_console_pixel(0xabcdef, format));
    assert(storage[0] == 0xdeadbeef && storage[n + 1] == 0xdeadbeef);
    console.format = 7;
    assert(!shz_boot_console_valid(&console));
    shz_boot_console_draw(&console, "ignored", "unsupported format", 0);
    assert(storage[1] == shz_boot_console_pixel(0x123456, format));
    console.format = format; console.pitch_pixels = width - 1;
    assert(!shz_boot_console_valid(&console));
    free(storage);
}
int main(void)
{
    assert(shz_boot_lint0_extint(0x10000) == 0x700); /* masked firmware handoff */
    assert(shz_boot_lint0_extint(0x1a045) == 0x700); /* wrong vector/polarity/trigger */
    assert(shz_boot_lint0_extint(0x700) == 0x700);   /* already virtual wire */
    test_uart(0); test_uart(1); test_uart(2);
    test_framebuffer(SHZ_BOOT_CONSOLE_RGBX, 640, 480);
    test_framebuffer(SHZ_BOOT_CONSOLE_BGRX, 640, 480);
    test_framebuffer(SHZ_BOOT_CONSOLE_BGRX, 160, 96);
    puts("boot devices PASS: RGBX/BGRX, pitch/edge guards, absent/stalled COM1, E9 and masked LINT0 handoff");
    return 0;
}
