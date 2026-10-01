/* SPDX-License-Identifier: GPL-2.0-only
 * Portable, bounds-checked MZ16 and fixed-base static PE32 image validation.
 * Shared with the host negative tests; no guest hardware instructions here.
 */
#ifndef SHZ_CPU_MODES_FORMATS_H
#define SHZ_CPU_MODES_FORMATS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t header, body;
    uint16_t ip, cs, sp, ss, reloc_count, reloc_table;
} cpu_mz_info;
typedef struct {
    uint32_t image_size, headers, entry;
    uint16_t sections;
    uint32_t section_table;
} cpu_pe32_info;

static inline uint16_t cpu_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static inline uint32_t cpu_u32(const uint8_t *p)
{ return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline int cpu_span(size_t n, uint32_t off, uint32_t len)
{ return off <= n && len <= n - off; }

static inline int cpu_mz_validate(const uint8_t *p, size_t n, cpu_mz_info *m)
{
    uint32_t expected, header, body, i;
    uint16_t last, pages, count, table;
    if (!p || !m || n < 32 || p[0] != 'M' || p[1] != 'Z') return -1;
    last = cpu_u16(p + 2); pages = cpu_u16(p + 4);
    if (!pages || last > 511 || cpu_u16(p + 26)) return -1;
    expected = ((uint32_t)pages - (last != 0)) * 512 + last;
    header = (uint32_t)cpu_u16(p + 8) * 16;
    count = cpu_u16(p + 6); table = cpu_u16(p + 24);
    if (expected != n || header < 32 || header >= n || count > 64 || table < 28 ||
        !cpu_span(header, table, (uint32_t)count * 4)) return -1;
    body = (uint32_t)n - header;
    if (body > 0xe000 || (uint32_t)cpu_u16(p + 22) * 16 + cpu_u16(p + 20) >= body ||
        (uint32_t)cpu_u16(p + 14) * 16 + cpu_u16(p + 16) < 16 ||
        (uint32_t)cpu_u16(p + 14) * 16 + cpu_u16(p + 16) >= 0x1fef0) return -1;
    for (i = 0; i < count; ++i) {
        const uint8_t *r = p + table + i * 4;
        uint32_t at = (uint32_t)cpu_u16(r + 2) * 16 + cpu_u16(r);
        if (!cpu_span(body, at, 2)) return -1;
    }
    m->header = header; m->body = body; m->ip = cpu_u16(p + 20); m->cs = cpu_u16(p + 22);
    m->sp = cpu_u16(p + 16); m->ss = cpu_u16(p + 14);
    m->reloc_count = count; m->reloc_table = table;
    return 0;
}

static inline int cpu_pe32_validate(const uint8_t *p, size_t n, cpu_pe32_info *m)
{
    uint32_t pe, opt, sect, image, headers, entry, i, dirs;
    uint16_t count, optbytes;
    int executable_entry = 0;
    if (!p || !m || n < 64 || p[0] != 'M' || p[1] != 'Z') return -1;
    pe = cpu_u32(p + 60);
    if (!cpu_span(n, pe, 24) || cpu_u32(p + pe) != 0x00004550 || cpu_u16(p + pe + 4) != 0x014c)
        return -1;
    count = cpu_u16(p + pe + 6); optbytes = cpu_u16(p + pe + 20); opt = pe + 24;
    if (!count || count > 4 || optbytes < 96 || !cpu_span(n, opt, optbytes) || cpu_u16(p + opt) != 0x010b)
        return -1;
    if (cpu_u32(p + opt + 28) != 0x40000 || cpu_u32(p + opt + 32) != 4096 ||
        cpu_u32(p + opt + 36) != 512) return -1;
    image = cpu_u32(p + opt + 56); headers = cpu_u32(p + opt + 60); entry = cpu_u32(p + opt + 16);
    dirs = cpu_u32(p + opt + 92);
    if (image < 4096 || image > 0x20000 || (image & 4095) || headers > image || headers > n ||
        !headers || entry >= image || dirs > 16 || optbytes < 96 + dirs * 8) return -1;
    /* This compatibility ABI deliberately has no Windows imports, TLS, or
     * loader-side relocation callbacks. Any data directory is refused. */
    for (i = 0; i < dirs; ++i)
        if (cpu_u32(p + opt + 96 + i * 8) || cpu_u32(p + opt + 100 + i * 8)) return -1;
    sect = opt + optbytes;
    if (!cpu_span(headers, sect, (uint32_t)count * 40)) return -1;
    for (i = 0; i < count; ++i) {
        const uint8_t *s = p + sect + i * 40;
        uint32_t virtual_size = cpu_u32(s + 8), va = cpu_u32(s + 12);
        uint32_t raw = cpu_u32(s + 16), pos = cpu_u32(s + 20), extent = virtual_size > raw ? virtual_size : raw;
        uint32_t j;
        if (va < headers || (va & 4095) || !cpu_span(image, va, extent) || !cpu_span(n, pos, raw) ||
            (raw && (pos < headers || (pos & 511)))) return -1;
        for (j = 0; j < i; ++j) {
            const uint8_t *other = p + sect + j * 40;
            uint32_t ov = cpu_u32(other + 12), os = cpu_u32(other + 8), ors = cpu_u32(other + 16);
            if (ors > os) os = ors;
            if (va < ov + os && ov < va + extent) return -1;
        }
        if ((cpu_u32(s + 36) & 0x20000000u) && entry >= va && entry - va < extent) executable_entry = 1;
    }
    if (!executable_entry) return -1;
    m->image_size = image; m->headers = headers; m->entry = entry;
    m->sections = count; m->section_table = sect;
    return 0;
}
#endif
