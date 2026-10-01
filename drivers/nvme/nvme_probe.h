#ifndef W98_NVME_PROBE_H
#define W98_NVME_PROBE_H

/* GPL-2.0-only. Read-only PCI/NVMe discovery for DOS and Win98 lab tools.
 * The caller supplies PCI enumeration/config access and an already validated
 * MMIO mapping. No device register or PCI configuration write is performed. */
#include <limits.h>
#if ULONG_MAX == 0xFFFFFFFFUL
typedef unsigned long w98_nvme_u32;
#elif UINT_MAX == 0xFFFFFFFFU
typedef unsigned int w98_nvme_u32;
#else
#error No 32-bit unsigned C integer type is available
#endif
typedef char w98_nvme_u32_is_four_bytes[(sizeof(w98_nvme_u32) == 4) ? 1 : -1];

#define W98_NVME_PCI_CLASS_CODE 0x010802UL
#define W98_NVME_MAX_CLASS_INDEX 256U

struct w98_nvme_bdf {
    unsigned char bus;
    unsigned char device;
    unsigned char function;
};

/* Return 1 for a match/read, 0 for no more matches/read failure, or a
 * negative value for an enumeration error. PCI class uses 01:08:02. */
typedef int (*w98_nvme_find_class)(void *context, w98_nvme_u32 class_code,
                                   unsigned int index, struct w98_nvme_bdf *bdf);
typedef int (*w98_nvme_pci_read32)(void *context,
                                   const struct w98_nvme_bdf *bdf,
                                   unsigned int byte_offset,
                                   w98_nvme_u32 *value);
typedef int (*w98_nvme_mmio_read32)(void *context, w98_nvme_u32 byte_offset,
                                    w98_nvme_u32 *value);

enum w98_nvme_status {
    W98_NVME_OK = 0,
    W98_NVME_NOT_FOUND = 1,
    W98_NVME_BAD_ARGUMENT = -1,
    W98_NVME_PCI_ERROR = -2,
    W98_NVME_BAD_CLASS = -3,
    W98_NVME_BAD_BAR = -4,
    W98_NVME_MEMORY_DISABLED = -6,
    W98_NVME_BAR_ABOVE_4G = -7,
    W98_NVME_REGION_TOO_SMALL = -8,
    W98_NVME_MMIO_ERROR = -9,
    W98_NVME_BAD_CAPABILITY = -10
};

struct w98_nvme_pci_info {
    struct w98_nvme_bdf bdf;
    unsigned int vendor_id;
    unsigned int device_id;
    unsigned int revision_id;
    unsigned int memory_space_enabled;
    unsigned int bar_is_64bit;
    w98_nvme_u32 bar_base_low;
    w98_nvme_u32 bar_base_high;
};

struct w98_nvme_capability {
    w98_nvme_u32 cap_low;
    w98_nvme_u32 cap_high;
    w98_nvme_u32 version_raw;
    w98_nvme_u32 max_queue_entries;
    unsigned int doorbell_stride_shift;
    unsigned int timeout_500ms_units;
    unsigned int min_memory_page_shift;
    unsigned int max_memory_page_shift;
    unsigned int version_major;
    unsigned int version_minor;
    unsigned int version_tertiary;
};

/* Probe one bounded BIOS/OS class enumeration index (0..255).
 * The callbacks need only read PCI config dwords at 00,04,08,0C,10 and 14.
 * Failure leaves *info unchanged. BAR sizing writes are intentionally absent. */
int w98_nvme_discover_index(w98_nvme_find_class find_class,
                            w98_nvme_pci_read32 pci_read32,
                            void *context, unsigned int candidate_index,
                            struct w98_nvme_pci_info *info);

/* Only reads CAP at 00/04 and VS at 08 after all local safety checks.
 * mapped_bytes must be the verified PCI resource length, not a guessed size.
 * An above-4GiB BAR is held back until a proven mapper is supplied. */
int w98_nvme_probe_mmio(const struct w98_nvme_pci_info *pci,
                        w98_nvme_mmio_read32 mmio_read32,
                        void *context, w98_nvme_u32 mapped_bytes,
                        struct w98_nvme_capability *capability);

#endif
