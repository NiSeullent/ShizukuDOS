/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHZ_CPU_MODES_H
#define SHZ_CPU_MODES_H
#include <stdint.h>

/* Native legacy execution is synchronous and privileged. Only the supplied,
 * audited, finite samples are accepted by the public launcher. */
#define CPU_MODES_LOW_BASE 0x10000u
#define CPU_MODES_LOW_BYTES 0x60000u
int cpu_modes_init(void);
unsigned cpu_modes_run_samples(void);
int cpu_modes_launch_named(const char *name);
unsigned cpu_modes_completed(void);

#endif
