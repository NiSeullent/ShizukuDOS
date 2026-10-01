/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KURAZY_API_H
#define KURAZY_API_H
#include <stdint.h>
#include <stddef.h>
#define KURAZY_ABI_VERSION 0x00010000u
#define KURAZY_MODE_REAL16 1u
#define KURAZY_MODE_PROTECTED32 2u
#define KURAZY_MODE_LONG64 4u
#define KURAZY_CAP_CONSOLE 1u
#define KURAZY_CAP_MEMORY 2u
#define KURAZY_CAP_FILES 4u
#define KURAZY_CAP_THREADS 8u
#define KURAZY_CAP_GOP 16u
#define KURAZY_CAP_LOCAL_BROWSER 32u
#define KURAZY_CAP_PCM_DECODE 64u
#define KURAZY_OK 0
#define KURAZY_E_ARGUMENT (-1)
#define KURAZY_E_ACCESS (-2)
#define KURAZY_E_NOT_FOUND (-3)
#define KURAZY_E_MEMORY (-4)
#define KURAZY_E_UNSUPPORTED (-5)
#define KURAZY_E_TIMEOUT (-6)
#define KURAZY_E_OWNER (-7)
#define KURAZY_E_LIMIT (-8)
#define KURAZY_E_CANCELLED (-9)
#define KURAZY_SC_QUERY 0x7000u
#define KURAZY_SC_WRITE 0x7001u
#define KURAZY_SC_TICKS 0x7002u
#define KURAZY_SC_ALLOC 0x7003u
#define KURAZY_SC_FREE 0x7004u
#define KURAZY_SC_READ 0x7005u
#define KURAZY_SC_DIR 0x7006u
#define KURAZY_SC_SPAWN 0x7007u
#define KURAZY_SC_JOIN 0x7008u
#define KURAZY_SC_CANCEL 0x7009u
#define KURAZY_SC_TREE 0x700au
#define KURAZY_SC_SLEEP 0x700bu
#define KURAZY_SC_EXIT 0x700cu
#define KURAZY_SC_GUI 0x700du
#define KURAZY_SC_SELF 0x700eu
#define KURAZY_TREE_RUNNING 1u
#define KURAZY_TREE_EXITED 2u
#define KURAZY_TREE_CANCELLED 4u
/* ABI v1 uses natural 8-byte alignment, little endian, no pointers in outputs. */
typedef struct {
    uint32_t size, version, mode, capabilities;
    uint64_t free_pages, total_pages, ticks_ms, thread_id;
    uint64_t cr0, efer;
    uint32_t framebuffer_width, framebuffer_height;
} kurazy_info;
typedef struct { char name[128]; uint64_t bytes; uint32_t directory, reserved; } kurazy_dirent;
typedef struct { uint64_t tid, parent_tid; int64_t exit_code; uint32_t state, reserved; } kurazy_thread_info;
typedef void (*kurazy_thread_fn)(void *argument);
int kurazy_query(kurazy_info *info);
int kurazy_write(const void *data, uint32_t bytes);
int kurazy_puts(const char *text);
void kurazy_u64(uint64_t value);
int kurazy_ticks(uint64_t *milliseconds);
int kurazy_alloc(uint64_t bytes, void **address);
int kurazy_free(void *address);
int kurazy_read(const char *path, void *buffer, uint32_t capacity, uint64_t offset);
int kurazy_dir(const char *path, kurazy_dirent *entries, uint32_t capacity);
int kurazy_spawn(kurazy_thread_fn entry, void *argument, uint64_t *tid);
int kurazy_join(uint64_t tid, int64_t *exit_code, uint32_t timeout_ms);
int kurazy_cancel(uint64_t tid);
int kurazy_tree(kurazy_thread_info *entries, uint32_t capacity);
int kurazy_sleep(uint32_t milliseconds);
uint64_t kurazy_self(void);
int kurazy_gui_open(uint32_t kind);
void kurazy_exit(int code) __attribute__((noreturn));
#endif
