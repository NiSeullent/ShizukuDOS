/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K64_KURAZY_H
#define K64_KURAZY_H
#include "proc_internal.h"
#include "../../sdk/kurazy/include/kurazy.h"
int kurazy_syscall(process_t *p,struct regs *r,uint32_t num,uint64_t a,uint64_t b,uint64_t c,uint64_t d,int32_t *status);
void kurazy_forget_process(int pid);
unsigned kurazy_self_test(void);
uint64_t kurazy_tree_kernel_id(thread_t *t);
uint64_t kurazy_tree_register_kernel(thread_t *t,uint64_t parent_id);
unsigned kurazy_tree_snapshot(kurazy_thread_info *out,unsigned cap,int pid);
#endif
