/* SPDX-License-Identifier: GPL-2.0-only
 * Native ShizukuDOS launch path: import-free AMD64 PE32+ kurazy programs
 * and the earlier raw SD64 x86-64 ABI. Both execute in paged ring 3.
 */
#include "fs.h"
#include "dos64.h"
#include "kurazy.h"
#include "cpu_modes.h"
#include "../win64/pe_parse.h"

#define DOS64_MAX_IMAGE (1024 * 1024)
#define DOS64_TIMEOUT_MS 5000
#define DOS64_REAP_GRACE_MS 1000

int dos64_boot_requested(void)
{
    if (k64_cmdline_has("shz.dos64")) return 1;
#ifdef SHZ_STANDALONE
    /* A DOS-only archive is the default native profile even without a token. */
    return fs_lookup("\\SHZ\\DOS64\\HELLO64.SD64") && !fs_lookup("\\SHZ\\SYS64\\ntdll.dll");
#else
    return 0;
#endif
}


static int refuse_import(void *ctx,const char *dll,const char *name,uint16_t ord,int by_ord,uint32_t iat)
{
    (void)ctx;(void)name;(void)iat;
    kprintf("NATIVE64: DLL dependency refused: %s ordinal=%u by_ordinal=%d\n",dll,ord,by_ord);
    return -1;
}
static int create_native_pe(const char *name,const uint8_t *image,uint64_t size,int *pid)
{
    pe_info_t info;process_t *p=0;int rc;uint64_t base,length,off;uint32_t old;
    rc=pe_parse(image,size,&info);
    if(rc || !(info.characteristics&PE_CHAR_EXECUTABLE) || (info.characteristics&PE_CHAR_DLL) ||
       !info.entry_rva || info.low_alignment || info.size_of_image>1024*1024 ||
       info.image_base<USER_MIN || info.image_base>USER_TOP-info.size_of_image ||
       (info.image_base&0xffff) || info.dir_size[9] || info.dir_size[10] || info.dir_size[13] || info.dir_size[14] ||
       pe_walk_imports(image,size,&info,refuse_import,0)) {
        kprintf("NATIVE64: %s refused: invalid image or unsupported DLL/TLS/runtime dependency (parser=%d)\n",name,rc);return -1;
    }
    {
        int executable_entry=0;
        for(unsigned i=0;i<info.nsections;i++) {
            pe_section_t sec;pe_get_section(image,&info,i,&sec);
            uint64_t end=(uint64_t)sec.rva+(sec.vsize>sec.raw_size?sec.vsize:sec.raw_size);
            if(info.entry_rva>=sec.rva&&info.entry_rva<end&&(sec.characteristics&PE_SCN_MEM_EXECUTE))executable_entry=1;
            if((sec.characteristics&PE_SCN_MEM_EXECUTE)&&(sec.characteristics&PE_SCN_MEM_WRITE))return -1;
        }
        if(!executable_entry){kprintf("NATIVE64: %s refused: entry is outside executable sections\n",name);return -1;}
    }
    p=process_create_empty(name);if(!p)return -1;proc_alloc_peb(p);
    base=info.image_base;length=info.size_of_image;
    if(vad_alloc(p,&base,&length,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE,VK_IMAGE))goto fail;
    /* Eager private pages: no Windows loader, import runtime, or lazy-image callbacks. */
    for(off=0;off<length;off+=4096){uint64_t pa=pmm_alloc();if(!pa)goto fail;if(vm_map(p->pml4,base+off,pa,prot_to_ptflags(PAGE_READWRITE))){pmm_free(pa);goto fail;}}
    if(image_poke(p,base,image,info.size_of_headers))goto fail;
    for(unsigned i=0;i<info.nsections;i++) {
        pe_section_t sec;pe_get_section(image,&info,i,&sec);
        if(sec.raw_size&&image_poke(p,base+sec.rva,image+sec.raw_off,sec.raw_size))goto fail;
    }
    /* Mark headers read-only and each section with its declared permissions. */
    {uint64_t a=base,n=(info.size_of_headers+4095)&~4095ull;if(vad_protect(p,&a,&n,PAGE_READONLY,&old))goto fail;}
    for(unsigned i=0;i<info.nsections;i++) {
        pe_section_t sec;uint32_t prot;uint64_t a,n;pe_get_section(image,&info,i,&sec);
        if(!sec.vsize&&!sec.raw_size)continue;
        if((sec.characteristics&PE_SCN_MEM_EXECUTE)&&(sec.characteristics&PE_SCN_MEM_WRITE))goto fail;
        prot=(sec.characteristics&PE_SCN_MEM_EXECUTE)?PAGE_EXECUTE_READ:(sec.characteristics&PE_SCN_MEM_WRITE)?PAGE_READWRITE:PAGE_READONLY;
        a=base+sec.rva;n=(sec.vsize>sec.raw_size?sec.vsize:sec.raw_size);if(vad_protect(p,&a,&n,prot,&old))goto fail;
    }
    p->image_base=base;p->entry=base+info.entry_rva;
    if(process_start_thread(p,p->entry,0,0,0))goto fail;
    *pid=p->pid;return 0;
fail:
    process_terminate(p,STATUS_INVALID_IMAGE_FORMAT,0);process_teardown(p);p->object->signaled=1;ob_deref(p->object);return -1;
}

static unsigned native_loader_negative_tests(void)
{
    fsnode_t *n=fs_lookup("\\SHZ\\DOS64\\HELLO64.EXE");
    uint8_t *original,*bad;uint64_t got=0;unsigned failures=0,checks=0;pe_info_t info;int pid;
    if(!n||n->size>1024*1024)return 1;
    original=kzalloc(n->size);bad=kzalloc(n->size);
    if(!original||!bad){kfree(original);kfree(bad);return 1;}
    if(fs_read(n,0,original,n->size,&got)||got!=n->size||pe_parse(original,n->size,&info)){kfree(original);kfree(bad);return 1;}
#define REJECT_CASE(label,sz) do{pid=0;if(!create_native_pe(label,bad,(sz),&pid)||pid)failures++;checks++;}while(0)
    memcpy(bad,original,n->size);REJECT_CASE("TRUNCATED64.EXE",64);
    memcpy(bad,original,n->size);*(uint32_t *)(bad+0x3c)=0xfffffff0u;REJECT_CASE("BADHEADER64.EXE",n->size);
    memcpy(bad,original,n->size);*(uint16_t *)(bad+info.nt_offset+4)=0x14c;REJECT_CASE("WRONGCPU64.EXE",n->size);
    memcpy(bad,original,n->size);*(uint32_t *)(bad+info.nt_offset+24+16)=info.size_of_image;REJECT_CASE("BADENTRY64.EXE",n->size);
    {
        int found=0;
        for(unsigned i=0;i<info.nsections;i++) {
            pe_section_t sec;pe_get_section(original,&info,i,&sec);
            if(sec.vsize&&!(sec.characteristics&PE_SCN_MEM_EXECUTE)) {
                memcpy(bad,original,n->size);*(uint32_t *)(bad+info.nt_offset+24+16)=sec.rva;
                REJECT_CASE("NOEXECENTRY64.EXE",n->size);found=1;break;
            }
        }
        if(!found)failures++;
    }
    /* Add real DLL/ordinal imports to .idata's existing raw padding. GNU ld's
     * empty sentinel has VirtualSize=24, so its file padding must explicitly
     * become part of the mutated mapped section before the import walker can
     * reach our 96-byte descriptor/name/thunk fixture. No original bytes are
     * interpreted beyond the parser-validated file-backed section range. */
    {
        int found=0;
        for(unsigned i=0;i<info.nsections;i++) {
            pe_section_t sec;uint32_t delta;uint64_t off;pe_get_section(original,&info,i,&sec);
            if(!info.dir_rva[1]||info.dir_rva[1]<sec.rva)continue;
            delta=info.dir_rva[1]-sec.rva;
            if(delta>sec.raw_size||sec.raw_size-delta<96||delta+96>info.section_alignment)continue;
            off=(uint64_t)sec.raw_off+delta;
            if(off>n->size||n->size-off<96)continue;
            memcpy(bad,original,n->size);memset(bad+off,0,96);
            if(sec.vsize<delta+96)*(uint32_t *)(bad+info.section_table_offset+i*40+8)=delta+96;
            *(uint32_t *)(bad+off)=info.dir_rva[1]+64;
            *(uint32_t *)(bad+off+12)=info.dir_rva[1]+40;
            *(uint32_t *)(bad+off+16)=info.dir_rva[1]+64;
            memcpy(bad+off+40,"MISSING.DLL",12);
            *(uint64_t *)(bad+off+64)=0x8000000000000001ull;
            *(uint32_t *)(bad+info.nt_offset+24+112+8+4)=40;
            REJECT_CASE("DLLDEPEND64.EXE",n->size);found=1;break;
        }
        if(!found)failures++;
    }
#undef REJECT_CASE
    kfree(original);kfree(bad);
    kprintf("NATIVE64: malformed-image/dependency rejection %u checks, %u failure(s)\n",checks,failures);
    return failures;
}

static unsigned run_one(const char *name)
{
    char path[80];const char *prefix="\\SHZ\\DOS64\\";
    unsigned i=0,j=0,waited=0,grace=0;int pid=0,faulted=1,timed_out=0,reaped;
    int64_t code=-1;uint64_t got=0;uint8_t *image;fsnode_t *n;process_t *p;
    while(prefix[i]){path[i]=prefix[i];++i;}while(name[j]&&i+1<sizeof path)path[i++]=name[j++];path[i]=0;
    n=fs_lookup(path);
    if(!n||n->is_dir||!n->size||n->size>DOS64_MAX_IMAGE){kprintf("DOS64: %s invalid or missing native image\n",name);return 1;}
    image=kzalloc(n->size);if(!image){kprintf("DOS64: %s image allocation failed\n",name);return 1;}
    if(fs_read(n,0,image,n->size,&got)||got!=n->size){kfree(image);return 1;}
    if(n->size>=2&&image[0]=='M'&&image[1]=='Z') {
        kprintf("DOS64: starting %s, native AMD64 PE32+ ring 3, zero DLL dependencies, %llu bytes\n",name,n->size);
        reaped=create_native_pe(name,image,n->size,&pid);
    } else {
        kprintf("DOS64: starting %s, native x86-64 ring 3, entry=400000, %llu bytes\n",name,n->size);
        reaped=proc_create_flat(name,image,n->size,&pid);
    }
    kfree(image);
    if(reaped||!(p=process_by_pid(pid))){kprintf("DOS64: %s process creation failed\n",name);return 1;}
    while(!(p->terminated&&p->threads_alive==0&&p->teardown==2)&&waited<DOS64_TIMEOUT_MS){thread_sleep_ms(10);waited+=10;}
    if(!(p->terminated&&p->threads_alive==0&&p->teardown==2)) {
        timed_out=1;process_terminate(p,STATUS_TIMEOUT,1);
        while(!(p->threads_alive==0&&p->teardown==2)&&grace<DOS64_REAP_GRACE_MS){thread_sleep_ms(10);grace+=10;}
        if(p->threads_alive||p->teardown!=2){kprintf("DOS64: %s timeout=1; process did not reap within %u ms\n",name,grace);return 1;}
    }
    kurazy_forget_process(pid);
    reaped=proc_wait(pid,&code,&faulted);
    kprintf("DOS64: %s exit=%08x faulted=%d timeout=%d reaped=%d\n",name,(uint32_t)code,faulted,timed_out,reaped);
    return reaped!=0||code!=0||faulted||timed_out;
}
int dos64_launch_named(const char *name)
{
    /* Restrict launcher to a filename in this application directory. */
    unsigned len=0;while(name[len]){char c=name[len];if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_')||len>=31)return 1;len++;}
    if(len<5)return 1;
    static const char *modes[]={"HELLO.EXE","MODE.EXE","COUNT.EXE","HELLO32.EXE","MODE32.EXE","COUNT32.EXE"};
    for(unsigned i=0;i<sizeof modes/sizeof modes[0];i++)if(!strcmp(name,modes[i]))return cpu_modes_launch_named(name);
    return (int)run_one(name);
}
unsigned dos64_run_samples(void)
{
    static const char *apps[]={"HELLO64.EXE","MEM64.EXE","TIME64.EXE","DIR64.EXE","TYPE64.EXE","HASH64.EXE","INFO64.EXE","THREAD64.EXE","TREE64.EXE","CHECK64.EXE","CPU64.EXE","VIDEO64.EXE","MUSIC64.EXE","BROWSE64.EXE"};
    unsigned failures=0,count=0;k64_boot_fb_t fb;int gop=!k64_boot_framebuffer(&fb);
    kprintf("ShizukuDOS: standalone native DOS64 application track (SD64 ABI v1 + kurazy v1 PE32+)\n");
    if(fs_lookup("\\SHZ\\DOS64\\HELLO64.EXE")) {
        failures+=kurazy_self_test();
        failures+=native_loader_negative_tests();
        for(unsigned i=0;i<sizeof apps/sizeof apps[0];i++) {
            if(i>=11&&!gop){kprintf("DOS64: %s requires GOP; skipped on BIOS text boot\n",apps[i]);continue;}
            failures+=run_one(apps[i]);count++;
        }
    }
    failures+=run_one("HELLO64.SD64");count++;
    failures+=run_one("MEM64.SD64");count++;
    kprintf("DOS64: completed %u application(s), %u failure(s)\n",count,failures);return failures;
}
