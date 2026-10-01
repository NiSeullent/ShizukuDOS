/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
int app_main(void) { uint64_t a=0,b=0;if(kurazy_ticks(&a)||kurazy_sleep(12)||kurazy_ticks(&b)||b-a<10||b-a>5000)return 1;kurazy_puts("TIME64: timer elapsed ms=");kurazy_u64(b-a);kurazy_puts(" PASS\n");return 0; }
