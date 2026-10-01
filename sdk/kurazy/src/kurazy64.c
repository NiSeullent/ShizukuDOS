/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy.h"
extern int64_t kurazy_call(uint64_t number, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
#define CALL(n,a,b,c,d) ((int)kurazy_call(n,(uint64_t)(a),(uint64_t)(b),(uint64_t)(c),(uint64_t)(d)))
int kurazy_query(kurazy_info *i) { return CALL(KURAZY_SC_QUERY,i,sizeof *i,0,0); }
int kurazy_write(const void *p,uint32_t n) { return CALL(KURAZY_SC_WRITE,p,n,0,0); }
int kurazy_puts(const char *p) { uint32_t n=0; while(p[n]) ++n; return kurazy_write(p,n); }
void kurazy_u64(uint64_t v) { char b[24]; unsigned n=0; do { b[n++]=(char)('0'+v%10); v/=10; } while(v); for(unsigned i=0;i<n/2;i++){ char t=b[i]; b[i]=b[n-1-i]; b[n-1-i]=t; } kurazy_write(b,n); }
int kurazy_ticks(uint64_t *v) { return CALL(KURAZY_SC_TICKS,v,0,0,0); }
int kurazy_alloc(uint64_t n,void **p) { return CALL(KURAZY_SC_ALLOC,n,p,0,0); }
int kurazy_free(void *p) { return CALL(KURAZY_SC_FREE,p,0,0,0); }
int kurazy_read(const char *p,void *b,uint32_t n,uint64_t o) { return CALL(KURAZY_SC_READ,p,b,n,o); }
int kurazy_dir(const char *p,kurazy_dirent *e,uint32_t n) { return CALL(KURAZY_SC_DIR,p,e,n,0); }
int kurazy_spawn(kurazy_thread_fn f,void *a,uint64_t *t) { return CALL(KURAZY_SC_SPAWN,f,a,t,0); }
int kurazy_join(uint64_t t,int64_t *c,uint32_t m) { return CALL(KURAZY_SC_JOIN,t,c,m,0); }
int kurazy_cancel(uint64_t t) { return CALL(KURAZY_SC_CANCEL,t,0,0,0); }
int kurazy_tree(kurazy_thread_info *e,uint32_t n) { return CALL(KURAZY_SC_TREE,e,n,0,0); }
int kurazy_sleep(uint32_t m) { return CALL(KURAZY_SC_SLEEP,m,0,0,0); }
uint64_t kurazy_self(void) { uint64_t t=0; (void)CALL(KURAZY_SC_SELF,&t,0,0,0); return t; }
int kurazy_gui_open(uint32_t k) { return CALL(KURAZY_SC_GUI,k,0,0,0); }
void kurazy_exit(int c) { (void)CALL(KURAZY_SC_EXIT,c,0,0,0); for(;;) __asm__ volatile("pause"); }
/* Freestanding helpers: never pull a Windows CRT into native binaries. */
void *memcpy(void *d,const void *s,size_t n){char *a=d;const char *b=s;for(size_t i=0;i<n;i++)a[i]=b[i];return d;}
void *memset(void *d,int c,size_t n){char *a=d;for(size_t i=0;i<n;i++)a[i]=(char)c;return d;}
