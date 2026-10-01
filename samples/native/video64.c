/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
int app_main(void){int r=kurazy_gui_open(1);kurazy_puts(r?"VIDEO64: GOP video unavailable\n":"VIDEO64: decoded packaged truecolor animation on GOP PASS\n");return r!=0;}
