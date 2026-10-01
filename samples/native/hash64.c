/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
static unsigned char buffer[1024];
static uint64_t fnv(const unsigned char *p,unsigned n,uint64_t h){for(unsigned i=0;i<n;i++)h=(h^p[i])*1099511628211ull;return h;}
int app_main(void) { if(fnv((const unsigned char*)"hello",5,14695981039346656037ull)!=0xa430d84680aabd0bull)return 1;uint64_t h=14695981039346656037ull,o=0;int n;while((n=kurazy_read("C:\\DOCS\\WELCOME.TXT",buffer,sizeof buffer,o))>0){h=fnv(buffer,n,h);o+=n;}if(n<0||!o)return 2;kurazy_puts("HASH64: FNV-1a64 file digest decimal=");kurazy_u64(h);kurazy_puts(" bytes=");kurazy_u64(o);kurazy_puts(" known-vector PASS\n");return 0; }
