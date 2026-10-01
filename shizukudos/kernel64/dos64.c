/* SPDX-License-Identifier: GPL-2.0-only
 * Native ShizukuDOS SD64 launch path. These raw x86-64 images enter ring 3
 * at 0x400000 and call the documented SD64 syscall ABI without any PE/DLL loader.
 */
#include "fs.h"
#include "dos64.h"

#define DOS64_MAX_IMAGE 65536
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

static unsigned run_one(const char *name)
{
    char path[80];
    const char *prefix = "\\SHZ\\DOS64\\";
    unsigned i = 0, j = 0, waited = 0, grace = 0;
    int pid = 0, faulted = 1, timed_out = 0, reaped;
    int64_t code = -1;
    uint64_t got = 0;
    uint8_t *image;
    fsnode_t *n;
    process_t *p;
    while (prefix[i]) { path[i] = prefix[i]; ++i; }
    while (name[j] && i + 1 < sizeof path) path[i++] = name[j++];
    path[i] = 0;
    n = fs_lookup(path);
    if (!n || n->is_dir || !n->size || n->size > DOS64_MAX_IMAGE) {
        kprintf("DOS64: %s invalid or missing native image\n", name);
        return 1;
    }
    image = kzalloc(n->size);
    if (!image) { kprintf("DOS64: %s image allocation failed\n", name); return 1; }
    if (fs_read(n, 0, image, n->size, &got) || got != n->size ||
        (n->size >= 2 && image[0] == 'M' && image[1] == 'Z')) {
        kprintf("DOS64: %s image read failed or PE/DOS MZ image refused\n", name);
        kfree(image);
        return 1;
    }
    kprintf("DOS64: starting %s, native x86-64 ring 3, entry=400000, %llu bytes\n", name, n->size);
    reaped = proc_create_flat(name, image, n->size, &pid);
    kfree(image);                         /* proc_create_flat copied every image page */
    if (reaped || !(p = process_by_pid(pid))) {
        kprintf("DOS64: %s process creation failed\n", name);
        return 1;
    }
    while (!(p->terminated && p->threads_alive == 0 && p->teardown == 2) && waited < DOS64_TIMEOUT_MS) {
        thread_sleep_ms(10);
        waited += 10;
    }
    if (!(p->terminated && p->threads_alive == 0 && p->teardown == 2)) {
        timed_out = 1;
        process_terminate(p, STATUS_TIMEOUT, 1);
        while (!(p->threads_alive == 0 && p->teardown == 2) && grace < DOS64_REAP_GRACE_MS) {
            thread_sleep_ms(10);
            grace += 10;
        }
        if (p->threads_alive || p->teardown != 2) {
            kprintf("DOS64: %s timeout=1; process did not reap within %u ms\n", name, grace);
            return 1;                     /* never enter the unbounded proc_wait path */
        }
    }
    reaped = proc_wait(pid, &code, &faulted);
    kprintf("DOS64: %s exit=%08x faulted=%d timeout=%d reaped=%d\n", name, (uint32_t)code,
            faulted, timed_out, reaped);
    return reaped != 0 || code != 0 || faulted || timed_out;
}

unsigned dos64_run_samples(void)
{
    unsigned failures = 0;
    kprintf("ShizukuDOS: standalone native DOS64 application track (SD64 ABI v1)\n");
    failures += run_one("HELLO64.SD64");
    failures += run_one("MEM64.SD64");
    kprintf("DOS64: completed 2 application(s), %u failure(s)\n", failures);
    return failures;
}
