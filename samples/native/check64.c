/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
extern int64_t kurazy_call(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
int app_main(void){uint64_t tid=0;char c;void *p=0;
if(kurazy_call(KURAZY_SC_QUERY,1,sizeof(kurazy_info),0,0)!=KURAZY_E_ACCESS)return 1;
if(kurazy_call(KURAZY_SC_QUERY,(uint64_t)&c,1,0,0)!=KURAZY_E_ARGUMENT)return 2;
if(kurazy_spawn((kurazy_thread_fn)(uintptr_t)1,0,&tid)!=KURAZY_E_ACCESS)return 3;
if(kurazy_alloc(0,&p)!=KURAZY_E_ARGUMENT)return 4;
if(kurazy_read("C:\\DOES-NOT-EXIST",&c,1,0)!=KURAZY_E_NOT_FOUND)return 5;
if(kurazy_gui_open(999)!=KURAZY_E_ARGUMENT)return 6;
kurazy_puts("CHECK64: bad pointer, bad size, non-executable entry, zero allocation, missing file, bad GUI kind PASS\n");return 0;}
