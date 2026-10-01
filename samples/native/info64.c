/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
int app_main(void) { kurazy_info i;if(kurazy_query(&i)||i.version!=KURAZY_ABI_VERSION||i.size!=sizeof i)return 1;kurazy_puts("INFO64: kurazy ABI 1.0 free/total pages=");kurazy_u64(i.free_pages);kurazy_puts("/");kurazy_u64(i.total_pages);kurazy_puts(" GOP=");kurazy_u64(i.framebuffer_width);kurazy_puts("x");kurazy_u64(i.framebuffer_height);kurazy_puts(" PASS\n");return 0; }
