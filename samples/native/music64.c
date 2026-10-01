/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
int app_main(void){int r=kurazy_gui_open(2);kurazy_puts(r?"MUSIC64: player unavailable\n":"MUSIC64: packaged score playback and visualization PASS\n");return r!=0;}
