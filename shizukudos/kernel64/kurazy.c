/* SPDX-License-Identifier: GPL-2.0-only
 * kurazy v1 native syscall service and owned thread trees.
 * Each app thread belongs to its creating thread. Cancelling/exiting a node
 * cancels all descendants; only the owning parent may join a direct child.
 */
#include "kurazy.h"
#include "fs.h"
#include "../../sdk/kurazy/include/kurazy.h"
#define TREE_CAP 128
#define IO_CAP 4096
struct tree_node { int used,pid; uint64_t tid,parent; thread_t *thread; int cancelled,joined; int64_t final_code; };
static struct tree_node tree[TREE_CAP];
int __attribute__((weak)) shizukugui_open(unsigned kind) { (void)kind; return KURAZY_E_UNSUPPORTED; }
static struct tree_node *find_node(int pid,uint64_t tid)
{
    for(unsigned i=0;i<TREE_CAP;i++) if(tree[i].used && tree[i].pid==pid && tree[i].tid==tid) return &tree[i];
    return 0;
}
static struct tree_node *insert_node(int pid,uint64_t tid,uint64_t parent,thread_t *t)
{
    struct tree_node *n=find_node(pid,tid);
    if(n) return n;
    for(unsigned i=0;i<TREE_CAP;i++) if(!tree[i].used) {
        n=&tree[i]; memset(n,0,sizeof *n); n->used=1;n->pid=pid;n->tid=tid;n->parent=parent;n->thread=t;return n;
    }
    return 0;
}
uint64_t kurazy_tree_kernel_id(thread_t *t) { return t?0x8000000000000000ull|t->id:0; }
uint64_t kurazy_tree_register_kernel(thread_t *t,uint64_t parent)
{
    const uint64_t f=irq_save(),id=kurazy_tree_kernel_id(t);
    struct tree_node *n=t?insert_node(0,id,parent,t):0;
    irq_restore(f); return n?id:0;
}
static int descendant(int pid,uint64_t child,uint64_t parent)
{
    for(unsigned depth=0;depth<TREE_CAP;depth++) {
        struct tree_node *n=find_node(pid,child);
        if(!n||!n->parent) return 0;
        if(n->parent==parent) return 1;
        child=n->parent;
    }
    return 0;
}
static void cancel_descendants(int pid,uint64_t root,int include_root)
{
    const uint64_t f=irq_save();
    for(unsigned i=0;i<TREE_CAP;i++) {
        struct tree_node *n=&tree[i]; thread_t *t=n->thread;
        if(!n->used||n->pid!=pid||(!descendant(pid,n->tid,root)&&!(include_root&&n->tid==root))) continue;
        n->cancelled=1;
        if(t && t->state!=TS_ZOMBIE && t->state!=TS_FREE) {
            t->kill_code=KURAZY_E_CANCELLED; t->kill_pending=1;
            if(t->state==TS_BLOCKED) thread_wake(t);
            if(t->state==TS_NEW) {t->suspend_count=0;thread_resume(t);}
        }
    }
    irq_restore(f);
}
unsigned kurazy_tree_snapshot(kurazy_thread_info *out,unsigned cap,int pid)
{
    unsigned count=0; const uint64_t f=irq_save();
    for(unsigned i=0;i<TREE_CAP && count<cap;i++) {
        struct tree_node *n=&tree[i];thread_t *t=n->thread;
        if(!n->used||(pid>=0&&n->pid!=pid)) continue;
        /* Kernel workers may have been reaped/recycled; IDs guard the pointer. */
        if(t && n->pid==0 && kurazy_tree_kernel_id(t)!=n->tid) {n->thread=0;t=0;}
        out[count].tid=n->tid;out[count].parent_tid=n->parent;
        out[count].exit_code=t?t->exit_code:n->final_code;
        out[count].state=(!t||t->state==TS_ZOMBIE||t->state==TS_FREE)?KURAZY_TREE_EXITED:KURAZY_TREE_RUNNING;
        if(n->cancelled) out[count].state|=KURAZY_TREE_CANCELLED;
        out[count].reserved=0;count++;
    }
    irq_restore(f);return count;
}
void kurazy_forget_process(int pid)
{
    const uint64_t f=irq_save();
    for(unsigned i=0;i<TREE_CAP;i++) if(tree[i].used&&tree[i].pid==pid) {
        if(tree[i].thread&&tree[i].parent&&!tree[i].joined) thread_creator_release(tree[i].thread);
        memset(&tree[i],0,sizeof tree[i]);
    }
    irq_restore(f);
}
static int user_path(process_t *p,uint64_t address,char *path)
{
    uint64_t len;
    if(user_string_len(p,address,255,&len)||len>=255||copy_from_user(p,path,address,len+1)) return KURAZY_E_ACCESS;
    return 0;
}
static int dispatch(process_t *p,uint32_t num,uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
    thread_t *cur=thread_current();struct tree_node *self;
    uint64_t f=irq_save();self=insert_node(p->pid,cur->tid,0,cur);irq_restore(f);
    if(!self) return KURAZY_E_LIMIT;
    switch(num) {
    case KURAZY_SC_QUERY: {
        kurazy_info i; k64_boot_fb_t fb; memset(&i,0,sizeof i);
        if(b!=sizeof i) return KURAZY_E_ARGUMENT;
        i.size=sizeof i;i.version=KURAZY_ABI_VERSION;i.mode=KURAZY_MODE_LONG64;
        i.capabilities=KURAZY_CAP_CONSOLE|KURAZY_CAP_MEMORY|KURAZY_CAP_FILES|KURAZY_CAP_THREADS;
        i.free_pages=pmm_free_count();i.total_pages=pmm_total_count();i.ticks_ms=ticks_now();i.thread_id=cur->tid;
        i.cr0=read_cr0();i.efer=rdmsr(MSR_EFER);
        if(!k64_boot_framebuffer(&fb)){ i.capabilities|=KURAZY_CAP_GOP|KURAZY_CAP_LOCAL_BROWSER; i.framebuffer_width=fb.width;i.framebuffer_height=fb.height; }
        return copy_to_user(p,a,&i,sizeof i)?KURAZY_E_ACCESS:0;
    }
    case KURAZY_SC_WRITE: {
        char buffer[IO_CAP+1];if(b>IO_CAP) return KURAZY_E_ARGUMENT;
        if(copy_from_user(p,buffer,a,b)) return KURAZY_E_ACCESS;
        buffer[b]=0;kprintf("%s",buffer);return (int)b;
    }
    case KURAZY_SC_TICKS: {uint64_t t=ticks_now();return copy_to_user(p,a,&t,8)?KURAZY_E_ACCESS:0;}
    case KURAZY_SC_SELF: {uint64_t t=cur->tid;return copy_to_user(p,a,&t,8)?KURAZY_E_ACCESS:0;}
    case KURAZY_SC_ALLOC: {
        uint64_t address=0,size=(a+4095)&~4095ull;
        if(!a||a>16*1024*1024) return KURAZY_E_ARGUMENT;
        if(vad_alloc(p,&address,&size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE,VK_PRIVATE)) return KURAZY_E_MEMORY;
        if(copy_to_user(p,b,&address,8)){uint64_t z=0;vad_free(p,&address,&z,MEM_RELEASE);return KURAZY_E_ACCESS;}return 0;
    }
    case KURAZY_SC_FREE: {
        vad_t *v=vad_find(p,a);uint64_t z=0;
        if(!v||v->kind!=VK_PRIVATE||v->alloc_base!=a) return KURAZY_E_ARGUMENT;
        return vad_free(p,&a,&z,MEM_RELEASE)?KURAZY_E_ARGUMENT:0;
    }
    case KURAZY_SC_READ: {
        char path[256];uint8_t buffer[IO_CAP];uint64_t got=0;fsnode_t *n;int rc;
        if(c>IO_CAP) return KURAZY_E_ARGUMENT;
        if((rc=user_path(p,a,path))) return rc;
        n=fs_lookup(path);if(!n||n->is_dir) return KURAZY_E_NOT_FOUND;
        if(fs_read(n,d,buffer,c,&got)||copy_to_user(p,b,buffer,got)) return KURAZY_E_ACCESS;
        return (int)got;
    }
    case KURAZY_SC_DIR: {
        char path[256];fsnode_t *dir,*n;unsigned count=0;int rc;
        if(c>64) return KURAZY_E_ARGUMENT;
        if((rc=user_path(p,a,path))) return rc;
        dir=fs_lookup(path);if(!dir||!dir->is_dir) return KURAZY_E_NOT_FOUND;fs_populate(dir);
        for(n=dir->child;n&&count<c;n=n->sibling){kurazy_dirent e;memset(&e,0,sizeof e);memcpy(e.name,n->name,strlen(n->name)+1);e.bytes=n->size;e.directory=n->is_dir;if(copy_to_user(p,b+count*sizeof e,&e,sizeof e))return KURAZY_E_ACCESS;count++;}return count;
    }
    case KURAZY_SC_SPAWN: {
        thread_t *t=0;uint64_t flags=0;struct tree_node *node;
        if(!vm_lookup(p->pml4,a,&flags)){if(user_fault_in(p,a,0,1))return KURAZY_E_ACCESS;vm_lookup(p->pml4,a,&flags);}
        if(!(flags&PT_U)||(flags&PT_NX))return KURAZY_E_ACCESS;
        if(copy_to_user(p,c,&flags,8))return KURAZY_E_ACCESS; /* validate output before making a child */
        if(process_start_thread3(p,a,b,0,65536,1,&t))return KURAZY_E_MEMORY;
        f=irq_save();node=insert_node(p->pid,t->tid,cur->tid,t);irq_restore(f);
        if(!node){t->kill_pending=1;t->kill_code=KURAZY_E_LIMIT;t->suspend_count=0;thread_resume(t);thread_creator_release(t);return KURAZY_E_LIMIT;}
        if(copy_to_user(p,c,&t->tid,8)){cancel_descendants(p->pid,t->tid,1);t->suspend_count=0;thread_resume(t);return KURAZY_E_ACCESS;}
        f=irq_save();t->suspend_count=0;thread_resume(t);irq_restore(f);return 0;
    }
    case KURAZY_SC_JOIN: {
        struct tree_node *n=find_node(p->pid,a);uint64_t start=ticks_now();int64_t code;
        if(!n)return KURAZY_E_NOT_FOUND;
        if(n->parent!=cur->tid)return KURAZY_E_OWNER;
        if(n->joined||c>5000)return KURAZY_E_ARGUMENT;
        while(n->thread->state!=TS_ZOMBIE){if(ticks_now()-start>=c)return KURAZY_E_TIMEOUT;thread_sleep_ms(1);}
        code=n->thread->exit_code;if(copy_to_user(p,b,&code,8))return KURAZY_E_ACCESS;
        {
            thread_t *joined=n->thread;const uint64_t lock=irq_save();
            n->joined=1;n->final_code=code;n->thread=0;
            thread_creator_release(joined); /* IDs survive; scheduler pointer cleared before release. */
            irq_restore(lock);
        }
        return 0;
    }
    case KURAZY_SC_CANCEL: {
        struct tree_node *n=find_node(p->pid,a);if(!n)return KURAZY_E_NOT_FOUND;
        if(n->parent!=cur->tid && !descendant(p->pid,a,cur->tid))return KURAZY_E_OWNER;
        cancel_descendants(p->pid,a,1);return 0;
    }
    case KURAZY_SC_TREE: {
        kurazy_thread_info *entries;unsigned count;if(b>64)return KURAZY_E_ARGUMENT;
        entries=kmalloc((b?b:1)*sizeof *entries);if(!entries)return KURAZY_E_MEMORY;
        count=kurazy_tree_snapshot(entries,b,p->pid);int rc=copy_to_user(p,a,entries,count*sizeof *entries)?KURAZY_E_ACCESS:(int)count;kfree(entries);return rc;
    }
    case KURAZY_SC_SLEEP: if(a>5000)return KURAZY_E_ARGUMENT;thread_sleep_ms(a);return 0;
    case KURAZY_SC_GUI: if(a<1||a>5)return KURAZY_E_ARGUMENT;return shizukugui_open(a);
    case KURAZY_SC_EXIT: {
        cancel_descendants(p->pid,cur->tid,0);
        /* Parent exit waits for its children to finish cancellation before its stack disappears. */
        uint64_t start=ticks_now();int alive;
        do{alive=0;for(unsigned i=0;i<TREE_CAP;i++)if(tree[i].thread&&tree[i].pid==p->pid&&descendant(p->pid,tree[i].tid,cur->tid)&&tree[i].thread->state!=TS_ZOMBIE)alive=1;if(alive)thread_sleep_ms(1);}while(alive&&ticks_now()-start<2000);
        cur->exit_code=(int32_t)a;process_thread_gone(p);thread_exit((int32_t)a);
    }
    default:return KURAZY_E_UNSUPPORTED;
    }
}
int kurazy_syscall(process_t *p,struct regs *r,uint32_t num,uint64_t a,uint64_t b,uint64_t c,uint64_t d,int32_t *status)
{
    (void)r;if(num<KURAZY_SC_QUERY||num>KURAZY_SC_SELF)return 0;
    *status=dispatch(p,num,a,b,c,d);return 1;
}
unsigned kurazy_self_test(void)
{
    /* Compile-time ABI layout consistency; runtime ownership checks run in THREAD64. */
    return sizeof(kurazy_info)!=72||sizeof(kurazy_thread_info)!=32||sizeof(kurazy_dirent)!=144;
}
