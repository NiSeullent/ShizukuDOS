/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K64_DOS64_H
#define K64_DOS64_H
/* Runs only native SD64 applications from \SHZ\DOS64; returns failures. */
unsigned dos64_run_samples(void);
/* DOS-only initrd automatically selects the focused standalone path. */
int dos64_boot_requested(void);
#endif
