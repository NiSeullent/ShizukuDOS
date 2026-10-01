/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
int app_main(void)
{
    uint32_t a, b, c, d;
    uint16_t cs, ss;
    char vendor[13];
    /* Check selectors returned by a real SYSCALL, rather than the initial IRETQ
     * entry frame. AMD SYSRET requires RPL 3 in the programmed STAR base. */
    (void)kurazy_self();
    __asm__ volatile("mov %%cs,%0; mov %%ss,%1" : "=r"(cs), "=r"(ss));
    if (cs != 0x23 || ss != 0x1b) {
        kurazy_puts("CPU64: invalid post-SYSCALL ring 3 selectors FAIL\n");
        return 1;
    }
    kurazy_puts("CPU64: post-SYSCALL CS=");
    kurazy_u64(cs);
    kurazy_puts(" SS=");
    kurazy_u64(ss);
    kurazy_puts(" PASS\n");
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    for (unsigned i = 0; i < 4; i++) {
        vendor[i] = (char)(b >> (i * 8));
        vendor[i + 4] = (char)(d >> (i * 8));
        vendor[i + 8] = (char)(c >> (i * 8));
    }
    vendor[12] = 0;
    kurazy_puts("CPU64: vendor=");
    kurazy_puts(vendor);
    kurazy_puts(" highest basic CPUID=");
    kurazy_u64(a);
    kurazy_puts(" native instruction PASS\n");
    return 0;
}
