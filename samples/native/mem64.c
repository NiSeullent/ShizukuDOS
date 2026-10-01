/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
int app_main(void) { unsigned char *p=0;if(kurazy_alloc(65536,(void**)&p)||!p)return 1;for(unsigned i=0;i<65536;i++)p[i]=(unsigned char)(i^0x5a);for(unsigned i=0;i<65536;i++)if(p[i]!=(unsigned char)(i^0x5a))return 2;if(kurazy_free(p))return 3;if(kurazy_free(p)!=KURAZY_E_ARGUMENT)return 4;kurazy_puts("MEM64: 65536 bytes demand-zero allocation, write/read/free PASS\n");return 0; }
