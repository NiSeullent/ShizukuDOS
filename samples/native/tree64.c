/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
static void worker(void *p){(void)p;kurazy_sleep(25);kurazy_exit(0);}
int app_main(void) { uint64_t a,b;int64_t c;kurazy_thread_info entries[16];if(kurazy_spawn(worker,0,&a)||kurazy_spawn(worker,0,&b))return 1;int n=kurazy_tree(entries,16),children=0;for(int i=0;i<n;i++)if(entries[i].parent_tid==kurazy_self())children++;if(children!=2)return 2;kurazy_puts("TREE64: root=");kurazy_u64(kurazy_self());kurazy_puts(" children=");kurazy_u64(children);kurazy_puts("\n");if(kurazy_join(a,&c,1000)||c||kurazy_join(b,&c,1000)||c)return 3;kurazy_puts("TREE64: owned concurrent sibling tasks PASS\n");return 0; }
