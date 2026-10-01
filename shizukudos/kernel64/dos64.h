/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K64_DOS64_H
#define K64_DOS64_H
/* Runs native PE32+ and compatibility SD64 applications; returns failures. */
unsigned dos64_run_samples(void);
int dos64_launch_named(const char *name);
/* DOS-only initrd automatically selects the focused standalone path. */
int dos64_boot_requested(void);
#endif
