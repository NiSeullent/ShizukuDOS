/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
static kurazy_dirent entries[32];
int app_main(void) { int n=kurazy_dir("C:\\SHZ\\DOS64",entries,32);if(n<2)return 1;kurazy_puts("DIR64: native application directory\n");for(int i=0;i<n;i++){kurazy_puts("  ");kurazy_puts(entries[i].name);kurazy_puts(" ");kurazy_u64(entries[i].bytes);kurazy_puts(" bytes\n");}kurazy_puts("DIR64: PASS\n");return 0; }
