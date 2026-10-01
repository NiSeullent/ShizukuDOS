/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHZ_BOOT_INTERRUPTS_H
#define SHZ_BOOT_INTERRUPTS_H
#include <stdint.h>
/* Intel SDM: virtual-wire LINT0 uses ExtINT; vector, polarity, trigger and
 * masking left by firmware must not select a different interrupt source. */
static inline uint32_t shz_boot_lint0_extint(uint32_t old)
{
    return (old & ~(UINT32_C(0xff) | UINT32_C(0x700) | UINT32_C(0x2000) |
                    UINT32_C(0x8000) | UINT32_C(0x10000))) | UINT32_C(0x700);
}
#endif
