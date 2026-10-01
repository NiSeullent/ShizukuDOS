/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
static volatile unsigned visits;
static volatile uint64_t branch_tid,leaf_tid;
static void leaf(void *arg){(void)arg;visits++;kurazy_exit(7);}
static void branch(void *arg){(void)arg;uint64_t tid;int64_t code;if(kurazy_spawn(leaf,0,&tid))kurazy_exit(10);leaf_tid=tid;if(kurazy_join(tid,&code,1000)||code!=7)kurazy_exit(11);visits++;kurazy_exit(9);}
static void waiting_leaf(void *arg){(void)arg;for(;;)kurazy_sleep(2);}
static void waiting_branch(void *arg){(void)arg;uint64_t tid;if(kurazy_spawn(waiting_leaf,0,&tid))kurazy_exit(12);leaf_tid=tid;for(;;)kurazy_sleep(2);}
int app_main(void) { uint64_t tid;int64_t code;kurazy_thread_info tree[16];if(kurazy_spawn(branch,0,&tid)||kurazy_join(tid,&code,1000)||code!=9||visits!=2)return 1;leaf_tid=0;if(kurazy_spawn(waiting_branch,0,&tid))return 2;branch_tid=tid;for(unsigned i=0;i<100&&!leaf_tid;i++)kurazy_sleep(1);if(!leaf_tid)return 3;int n=kurazy_tree(tree,16),edges=0;for(int i=0;i<n;i++)if(tree[i].tid==leaf_tid&&tree[i].parent_tid==branch_tid)edges++;if(edges!=1)return 4;if(kurazy_join(leaf_tid,&code,0)!=KURAZY_E_OWNER)return 5;if(kurazy_cancel(tid)||kurazy_join(tid,&code,1000)||code!=KURAZY_E_CANCELLED)return 6;for(unsigned i=0;i<50;i++)kurazy_sleep(1);n=kurazy_tree(tree,16);for(int i=0;i<n;i++)if(tree[i].tid==leaf_tid&&(!(tree[i].state&KURAZY_TREE_CANCELLED)||!(tree[i].state&KURAZY_TREE_EXITED)))return 7;kurazy_puts("THREAD64: real scheduled parent/child/grandchild, owner check, subtree cancellation + join PASS\n");return 0; }
