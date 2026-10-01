/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
static char text[1024];
int app_main(void) { int n=kurazy_read("C:\\DOCS\\WELCOME.TXT",text,sizeof text,0);if(n<1)return 1;kurazy_puts("TYPE64: ");kurazy_write(text,n);int eof=kurazy_read("C:\\DOCS\\WELCOME.TXT",text,1,1048576);if(eof!=0)return 2;kurazy_puts("TYPE64: file read + EOF PASS\n");return 0; }
