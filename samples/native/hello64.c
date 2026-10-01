/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
int app_main(void) { kurazy_info i; if(kurazy_query(&i)||i.mode!=KURAZY_MODE_LONG64||(i.cr0&0x80000001ull)!=0x80000001ull||!(i.efer&0x400))return 1;kurazy_puts("HELLO64: true PE32+ x86-64 ring 3; kurazy long mode PASS\n");return 0; }
