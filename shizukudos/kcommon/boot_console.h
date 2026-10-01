/* SPDX-License-Identifier: GPL-2.0-only
 * Allocation-free GOP boot/error panel, usable before or after ExitBootServices.
 * `base` must already be mapped in the caller's current address space. The
 * framebuffer format uses the Shizuku boot ABI's RGBX=1 / BGRX=2 values.
 */
#ifndef SHZ_BOOT_CONSOLE_H
#define SHZ_BOOT_CONSOLE_H
#include <stddef.h>
#include <stdint.h>
#include "../supervisor/src/font8x8_basic.h"
#define SHZ_BOOT_FB_VA UINT64_C(0xffffc00000000000)
#define SHZ_BOOT_CONSOLE_RGBX 1u
#define SHZ_BOOT_CONSOLE_BGRX 2u

typedef struct {
    volatile uint32_t *pixels;
    uint32_t width, height, pitch_pixels, format;
} shz_boot_console_t;
static inline uint32_t shz_boot_console_pixel(uint32_t c, uint32_t format)
{
    return format == SHZ_BOOT_CONSOLE_RGBX ?
        ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255) : c;
}
static inline int shz_boot_console_valid(const shz_boot_console_t *c)
{
    return c && c->pixels && c->width >= 160 && c->height >= 96 &&
        c->width <= 16384 && c->height <= 16384 && c->pitch_pixels >= c->width &&
        (c->format == SHZ_BOOT_CONSOLE_RGBX || c->format == SHZ_BOOT_CONSOLE_BGRX);
}
static inline void shz_boot_console_rect(const shz_boot_console_t *c, unsigned x, unsigned y,
                                         unsigned w, unsigned h, uint32_t color)
{
    unsigned row, col;
    if (x >= c->width || y >= c->height) return;
    if (w > c->width - x) w = c->width - x;
    if (h > c->height - y) h = c->height - y;
    color = shz_boot_console_pixel(color, c->format);
    for (row = 0; row < h; ++row)
        for (col = 0; col < w; ++col)
            c->pixels[(size_t)(y + row) * c->pitch_pixels + x + col] = color;
}
static inline void shz_boot_console_text(const shz_boot_console_t *c, unsigned x, unsigned y,
                                         const char *s, uint32_t color)
{
    unsigned row, col;
    color = shz_boot_console_pixel(color, c->format);
    if (x >= c->width || y >= c->height || !s) return;
    while (*s && c->width - x >= 8 && c->height - y >= 16) {
        unsigned ch = (uint8_t)*s++; if (ch >= 128) ch = '?';
        for (row = 0; row < 16; ++row)
            for (col = 0; col < 8; ++col)
                if ((font8x8_basic[ch][row >> 1] >> col) & 1)
                    c->pixels[(size_t)(y + row) * c->pitch_pixels + x + col] = color;
        x += 8;
    }
}
static inline void shz_boot_console_draw(const shz_boot_console_t *c, const char *stage,
                                         const char *detail, uint32_t color)
{
    unsigned h;
    if (!shz_boot_console_valid(c)) return;
    h = c->height < 192 ? c->height : 192;
    shz_boot_console_rect(c, 0, 0, c->width, h, 0x102c36);
    shz_boot_console_rect(c, 0, 0, 6, h, color);
    shz_boot_console_text(c, 20, 18, "SHIZUKUDOS / UEFI BOOT", 0xd4f4e9);
    shz_boot_console_text(c, 20, 47, stage ? stage : "STARTING", color);
    shz_boot_console_text(c, 20, 77, detail ? detail : "", 0xd4f4e9);
    if (h >= 160) {
        shz_boot_console_text(c, 20, 113, "Boot progress is visible here; errors remain on screen.", 0x92b3bb);
        shz_boot_console_text(c, 20, 140, "Debug output: COM1 or optional port E9 debug console.", 0x92b3bb);
    }
}
static inline void shz_boot_console_draw_at(uint64_t base, uint32_t width, uint32_t height,
                                            uint32_t pitch_pixels, uint32_t format,
                                            const char *stage, const char *detail, uint32_t color)
{
    shz_boot_console_t c = { (volatile uint32_t *)(uintptr_t)base, width, height, pitch_pixels, format };
    shz_boot_console_draw(&c, stage, detail, color);
}
#endif
