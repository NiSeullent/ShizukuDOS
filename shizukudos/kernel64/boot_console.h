/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K64_BOOT_CONSOLE_H
#define K64_BOOT_CONSOLE_H
#include "k64.h"
/* Dedicated framebuffer mapping avoids modifying the RAM direct map's large
 * leaves. May be called after mem_init(), before the scheduler exists. */
void *k64_boot_framebuffer_map(void);
void k64_boot_console_stage(const char *stage, const char *detail);
void k64_boot_console_fail(const char *stage, const char *detail);
void k64_boot_console_enable(int enabled);
void k64_boot_console_write(const char *bytes, size_t n);
void k64_boot_console_exit(unsigned code);
#endif
