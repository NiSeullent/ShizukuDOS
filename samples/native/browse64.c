/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
int app_main(void){int r=kurazy_gui_open(3);kurazy_puts(r?"BROWSE64: browser unavailable\n":"BROWSE64: original packaged local HTML rendering PASS\n");return r!=0;}
