/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../shizukudos/kernel64/cpu_modes_formats.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); return 1; } } while (0)
static void set16(uint8_t *p, uint16_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void set32(uint8_t *p, uint32_t x)
{ p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24); }
static size_t read_image(const char *name, uint8_t *p)
{
    char path[256]; FILE *f; size_t n;
    snprintf(path, sizeof path, "build/modes/%s", name);
    f = fopen(path, "rb"); if (!f) return 0;
    n = fread(p, 1, 65536, f); fclose(f); return n;
}
int main(void)
{
    uint8_t good[65536], bad[65536];
    const char *mz_names[] = {"HELLO.EXE", "MODE.EXE", "COUNT.EXE"};
    const char *pe_names[] = {"HELLO32.EXE", "MODE32.EXE", "COUNT32.EXE"};
    size_t n; unsigned i; cpu_mz_info mz; cpu_pe32_info pe;
    for (i = 0; i < 3; ++i) {
        n = read_image(mz_names[i], good); CHECK(n > 0); CHECK(!cpu_mz_validate(good, n, &mz));
        CHECK(cpu_mz_validate(good, 31, &mz));
        memcpy(bad, good, n); set16(bad+2, 512); CHECK(cpu_mz_validate(bad,n,&mz));
        memcpy(bad, good, n); set16(bad+8, 0xffff); CHECK(cpu_mz_validate(bad,n,&mz));
        memcpy(bad, good, n); set16(bad+24, 31); CHECK(cpu_mz_validate(bad,n,&mz));
        memcpy(bad, good, n); set16(bad+28, 0xffff); CHECK(cpu_mz_validate(bad,n,&mz));
        memcpy(bad, good, n); set16(bad+22, 0xffff); CHECK(cpu_mz_validate(bad,n,&mz));
        memcpy(bad, good, n); set16(bad+14, 0xffff); CHECK(cpu_mz_validate(bad,n,&mz));
    }
    for (i = 0; i < 3; ++i) {
        uint32_t h, opt, sect;
        n = read_image(pe_names[i], good); CHECK(n > 0); CHECK(!cpu_pe32_validate(good,n,&pe));
        h = cpu_u32(good+60); opt = h+24; sect = opt+cpu_u16(good+h+20);
        CHECK(cpu_pe32_validate(good,63,&pe));
        memcpy(bad,good,n); set32(bad+60,0xfffffff0); CHECK(cpu_pe32_validate(bad,n,&pe));
        memcpy(bad,good,n); set16(bad+h+4,0x8664); CHECK(cpu_pe32_validate(bad,n,&pe));
        memcpy(bad,good,n); set32(bad+opt+28,0x100000); CHECK(cpu_pe32_validate(bad,n,&pe));
        memcpy(bad,good,n); set32(bad+opt+104,0x1000); CHECK(cpu_pe32_validate(bad,n,&pe));
        memcpy(bad,good,n); set32(bad+sect+12,0x1ff00); CHECK(cpu_pe32_validate(bad,n,&pe));
        memcpy(bad,good,n); set32(bad+sect+20,0xfffffff0); CHECK(cpu_pe32_validate(bad,n,&pe));
        memcpy(bad,good,n); set32(bad+sect+36,0xc0000040); CHECK(cpu_pe32_validate(bad,n,&pe));
    }
    /* Truncated, arbitrary and mutated inputs must be memory safe even when
     * the parser quite correctly accepts some harmless header mutations. */
    n = read_image("HELLO32.EXE", good);
    for (i = 0; i < n; ++i) { (void)cpu_mz_validate(good,i,&mz); (void)cpu_pe32_validate(good,i,&pe); ++checks; }
    {
        uint32_t rng = 0x4b555241u;
        for (i = 0; i < 50000; ++i) {
            uint32_t off; memcpy(bad,good,n);
            rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; off = rng % (uint32_t)n;
            bad[off] ^= (uint8_t)(rng>>24);
            (void)cpu_pe32_validate(bad,n,&pe); (void)cpu_mz_validate(bad,n,&mz); ++checks;
        }
    }
    printf("MODE-FORMATS: %u checks PASS (valid MZ/PE32, rejected bounds/imports/machine, mutation safety)\n", checks);
    return 0;
}
