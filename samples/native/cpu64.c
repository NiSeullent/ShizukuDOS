/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
int app_main(void){uint32_t a,b,c,d;char vendor[13];__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0));for(unsigned i=0;i<4;i++){vendor[i]=(char)(b>>(i*8));vendor[i+4]=(char)(d>>(i*8));vendor[i+8]=(char)(c>>(i*8));}vendor[12]=0;kurazy_puts("CPU64: vendor=");kurazy_puts(vendor);kurazy_puts(" highest basic CPUID=");kurazy_u64(a);kurazy_puts(" native instruction PASS\n");return 0;}
