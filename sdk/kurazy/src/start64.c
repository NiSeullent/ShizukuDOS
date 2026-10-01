/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
extern int app_main(void);
void kurazy_start(void) { kurazy_exit(app_main()); }
