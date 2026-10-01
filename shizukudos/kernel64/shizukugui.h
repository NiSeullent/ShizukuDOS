/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIZUKUGUI_H
#define SHIZUKUGUI_H
/* A GOP-only desktop. F8 runs full native acceptance; F10 returns for shutdown. */
int shizukugui_init(void);
int shizukugui_selftest(void);
void shizukugui_run(void);
enum { SHIZUKUGUI_VIDEO = 1, SHIZUKUGUI_MUSIC = 2, SHIZUKUGUI_BROWSER = 3,
       SHIZUKUGUI_TREE = 4, SHIZUKUGUI_APPS = 5 };
/* Nonblocking pane activation for native kurazy applications; values above
 * are independent of the keyboard's F1=browser/F2=video/F3=music ordering. */
int shizukugui_open(unsigned kind);
#endif
