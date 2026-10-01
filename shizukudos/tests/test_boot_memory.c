/* SPDX-License-Identifier: GPL-2.0-only
 * Exercise the production allocator/page walker with RAM-backed GOP. Only
 * privileged CPU operations and physical address access are mocked; mem.c is
 * included unchanged. Two shared host mappings model the boot and RAM aliases.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../abi/shz_abi.h"
#include "../kcommon/boot_console.h"
#define K64_H 1
#define SHZ_STANDALONE 1
#define PAGE_SIZE UINT64_C(4096)
#define K64_VIRT_BASE UINT64_C(0x1000000000)
#define DIRECT_MAP UINT64_C(0x2000000000)
#define KWIN_BASE UINT64_C(0xffffc10000000000)
#define NTDRV_VA_BASE UINT64_C(0xffffe00000000000)
#define PT_P UINT64_C(1)
#define PT_W UINT64_C(2)
#define PT_U UINT64_C(4)
#define PT_PWT UINT64_C(8)
#define PT_PCD UINT64_C(16)
#define PT_NX (UINT64_C(1) << 63)
#define KASSERT(c) assert(c)
extern uint64_t phys_base_va;
static uint64_t p2v(uint64_t pa) { return phys_base_va + pa; }
static uint64_t irq_save(void) { return 0; }
static void irq_restore(uint64_t f) { (void)f; }
static void write_cr3(uint64_t pa) { assert(pa); }
static void invlpg(uint64_t va) { (void)va; }
static void kprintf(const char *format, ...) { (void)format; }
static void shz_evidence(unsigned slot, uint64_t value) { (void)slot; (void)value; }
#include "../kernel64/mem.c"

#define TEST_RAM (UINT64_C(64) << 20)
static void unchanged(uint64_t start, uint64_t bytes)
{
    const uint8_t *scanout = (const uint8_t *)(uintptr_t)(DIRECT_MAP + start);
    for (uint64_t i = 0; i < bytes; ++i) assert(scanout[i] == 0xa5);
}
static void scenario(uint64_t framebuffer, int heap)
{
    shz_bootinfo_t info = {0};
    shz_memholes_t *holes = (shz_memholes_t *)(uintptr_t)(K64_VIRT_BASE + SHZ_MEMHOLES_GPA);
    uint64_t pa, count = 0, bytes = UINT64_C(640) * 480 * 4;
    memset((void *)(uintptr_t)K64_VIRT_BASE, 0, TEST_RAM);
    phys_base_va = K64_VIRT_BASE;
    hole_count = 0; heap_total_bytes = 0; heap_used_bytes = 0; pmm_hint = 0;
    holes->magic = SHZ_MEMHOLES_MAGIC; holes->count = 1;
    holes->hole[0].gpa = UINT64_C(8) << 20; holes->hole[0].size = UINT64_C(1) << 20;
    holes->check = shz_memholes_sum(holes);
    info.size = sizeof info; info.ram_size = TEST_RAM;
    info.initrd_gpa = UINT64_C(32) << 20; info.initrd_size = UINT64_C(2) << 20;
    info.fb_base = framebuffer; info.fb_width = 640; info.fb_height = 480;
    info.fb_pitch = 640 * 4; info.fb_bpp = 32; info.fb_format = SHZ_FB_BGRX8888; info.fb_size = bytes;
    memset((void *)(uintptr_t)(K64_VIRT_BASE + framebuffer), 0xa5, (size_t)bytes);
    mem_init(&info);
    unchanged(framebuffer, bytes);
    /* The direct map contains a large leaf. It must never be interpreted as
     * a page table, even when a caller asks to change one small MMIO page. */
    assert(vm_map(kernel_pml4(), DIRECT_MAP + framebuffer, framebuffer, PT_W | PT_NX | PT_PCD) == -1);
    unchanged(framebuffer, bytes);
    assert(vm_map(kernel_pml4(), SHZ_BOOT_FB_VA, framebuffer, PT_W | PT_NX | PT_PCD | PT_PWT) == 0);
    assert(vm_lookup(kernel_pml4(), SHZ_BOOT_FB_VA, 0) == framebuffer);
    unchanged(framebuffer, bytes);
    if (heap) {
        void *block;
        while ((block = kmalloc(4096))) {
            uint64_t start = (uint64_t)(uintptr_t)block - DIRECT_MAP;
            assert(start + 4096 <= framebuffer || start >= framebuffer + bytes);
            memset(block, 0x5a, 4096); ++count;
        }
        assert(count > 1500);
    } else {
        while ((pa = pmm_alloc())) {
            assert(pa + PAGE_SIZE <= framebuffer || pa >= framebuffer + bytes);
            assert(pa + PAGE_SIZE <= info.initrd_gpa || pa >= info.initrd_gpa + info.initrd_size);
            ++count;
        }
        assert(count > 9000 && !pmm_free_count());
    }
    unchanged(framebuffer, bytes);
}
int main(void)
{
    int fd = memfd_create("shizukudos-boot-memory-test", 0);
    assert(fd >= 0 && !ftruncate(fd, (off_t)TEST_RAM));
    assert(mmap((void *)(uintptr_t)K64_VIRT_BASE, TEST_RAM, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0) == (void *)(uintptr_t)K64_VIRT_BASE);
    assert(mmap((void *)(uintptr_t)DIRECT_MAP, TEST_RAM, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0) == (void *)(uintptr_t)DIRECT_MAP);
    scenario(UINT64_C(24) << 20, 0);                   /* scanout inside page allocator */
    scenario((UINT64_C(7) << 20) + (UINT64_C(512) << 10), 1); /* overlapping firmware heap hole */
    munmap((void *)(uintptr_t)K64_VIRT_BASE, TEST_RAM);
    munmap((void *)(uintptr_t)DIRECT_MAP, TEST_RAM); close(fd);
    puts("boot memory PASS: actual allocators preserve RAM-backed GOP, overlapping heap holes, safe large leaves and dedicated mapping");
    return 0;
}
