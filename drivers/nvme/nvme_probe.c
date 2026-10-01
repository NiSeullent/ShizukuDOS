#include "nvme_probe.h"

/* PCI-SIG class 01:08:02; NVMe Base 2.1 CAP (00h, eight bytes) and VS (08h).
 * This module reads only. A real DOS bridge can use PCI BIOS INT 1Ah B103h
 * and B10Ah; a Win98 bridge should use the DDK's assigned PCI resources. */
#define PCI_VENDOR_DEVICE 0x00U
#define PCI_COMMAND       0x04U
#define PCI_CLASS_REV     0x08U
#define PCI_HEADER_TYPE   0x0CU
#define PCI_BAR0          0x10U
#define PCI_BAR1          0x14U

#define NVME_CAP_LOW      0x00UL
#define NVME_CAP_HIGH     0x04UL
#define NVME_VS           0x08UL
#define NVME_MIN_BYTES    0x0CUL

static int read_pci(w98_nvme_pci_read32 read32, void *context,
                    const struct w98_nvme_bdf *bdf, unsigned int offset,
                    w98_nvme_u32 *value)
{
    return read32(context, bdf, offset, value) == 1;
}

int w98_nvme_discover_index(w98_nvme_find_class find_class,
                            w98_nvme_pci_read32 pci_read32,
                            void *context, unsigned int candidate_index,
                            struct w98_nvme_pci_info *info)
{
    struct w98_nvme_pci_info found;
    w98_nvme_u32 value;
    w98_nvme_u32 class_rev;
    w98_nvme_u32 command;
    w98_nvme_u32 header;
    w98_nvme_u32 bar0;
    unsigned int memory_type;
    int search_result;

    if (!find_class || !pci_read32 || !info ||
        candidate_index >= W98_NVME_MAX_CLASS_INDEX)
        return W98_NVME_BAD_ARGUMENT;

    search_result = find_class(context, W98_NVME_PCI_CLASS_CODE,
                               candidate_index, &found.bdf);
    if (search_result == 0) return W98_NVME_NOT_FOUND;
    if (search_result != 1 || found.bdf.device >= 32U ||
        found.bdf.function >= 8U)
        return W98_NVME_PCI_ERROR;

    if (!read_pci(pci_read32, context, &found.bdf,
                  PCI_VENDOR_DEVICE, &value) ||
        value == 0xFFFFFFFFUL || (value & 0xFFFFUL) == 0UL ||
        (value & 0xFFFFUL) == 0xFFFFUL)
        return W98_NVME_PCI_ERROR;
    found.vendor_id = (unsigned int)(value & 0xFFFFUL);
    found.device_id = (unsigned int)((value >> 16) & 0xFFFFUL);

    if (!read_pci(pci_read32, context, &found.bdf,
                  PCI_CLASS_REV, &class_rev))
        return W98_NVME_PCI_ERROR;
    if (((class_rev >> 8) & 0xFFFFFFUL) != W98_NVME_PCI_CLASS_CODE)
        return W98_NVME_BAD_CLASS;
    found.revision_id = (unsigned int)(class_rev & 0xFFUL);

    if (!read_pci(pci_read32, context, &found.bdf,
                  PCI_COMMAND, &command) ||
        !read_pci(pci_read32, context, &found.bdf,
                  PCI_HEADER_TYPE, &header) ||
        !read_pci(pci_read32, context, &found.bdf,
                  PCI_BAR0, &bar0))
        return W98_NVME_PCI_ERROR;
    if (((header >> 16) & 0x7FUL) != 0U || (bar0 & 1UL) != 0UL)
        return W98_NVME_BAD_BAR;
    memory_type = (unsigned int)((bar0 >> 1) & 3UL);
    if (memory_type != 0U && memory_type != 2U)
        return W98_NVME_BAD_BAR;

    found.memory_space_enabled = (unsigned int)((command >> 1) & 1UL);
    found.bar_is_64bit = (memory_type == 2U) ? 1U : 0U;
    found.bar_base_low = bar0 & 0xFFFFFFF0UL;
    found.bar_base_high = 0UL;
    if (found.bar_is_64bit && !read_pci(pci_read32, context,
                                        &found.bdf, PCI_BAR1,
                                        &found.bar_base_high))
        return W98_NVME_PCI_ERROR;
    if (found.bar_base_low == 0UL && found.bar_base_high == 0UL)
        return W98_NVME_BAD_BAR;

    *info = found;
    return W98_NVME_OK;
}

int w98_nvme_probe_mmio(const struct w98_nvme_pci_info *pci,
                        w98_nvme_mmio_read32 mmio_read32,
                        void *context, w98_nvme_u32 mapped_bytes,
                        struct w98_nvme_capability *capability)
{
    struct w98_nvme_capability result;
    w98_nvme_u32 min_mps;
    w98_nvme_u32 max_mps;

    if (!pci || !mmio_read32 || !capability) return W98_NVME_BAD_ARGUMENT;
    if ((pci->bar_base_low == 0UL && pci->bar_base_high == 0UL) ||
        pci->bar_is_64bit > 1U ||
        (!pci->bar_is_64bit && pci->bar_base_high != 0UL))
        return W98_NVME_BAD_BAR;
    if (!pci->memory_space_enabled) return W98_NVME_MEMORY_DISABLED;
    if (pci->bar_base_high != 0UL) return W98_NVME_BAR_ABOVE_4G;
    if (mapped_bytes < NVME_MIN_BYTES) return W98_NVME_REGION_TOO_SMALL;

    if (mmio_read32(context, NVME_CAP_LOW, &result.cap_low) != 1 ||
        mmio_read32(context, NVME_CAP_HIGH, &result.cap_high) != 1 ||
        mmio_read32(context, NVME_VS, &result.version_raw) != 1)
        return W98_NVME_MMIO_ERROR;

    min_mps = (result.cap_high >> 16) & 0xFUL;
    max_mps = (result.cap_high >> 20) & 0xFUL;
    if ((result.cap_low == 0xFFFFFFFFUL &&
         result.cap_high == 0xFFFFFFFFUL) ||
        (result.cap_low & 0xFFFFUL) == 0UL || min_mps > max_mps ||
        result.version_raw == 0UL || result.version_raw == 0xFFFFFFFFUL ||
        (result.version_raw >> 16) == 0UL ||
        (result.version_raw >> 16) == 0xFFFFUL)
        return W98_NVME_BAD_CAPABILITY;

    result.max_queue_entries = (result.cap_low & 0xFFFFUL) + 1UL;
    result.timeout_500ms_units =
        (unsigned int)((result.cap_low >> 24) & 0xFFUL);
    result.doorbell_stride_shift =
        (unsigned int)(result.cap_high & 0xFUL);
    result.min_memory_page_shift = (unsigned int)min_mps + 12U;
    result.max_memory_page_shift = (unsigned int)max_mps + 12U;
    result.version_major = (unsigned int)(result.version_raw >> 16);
    result.version_minor =
        (unsigned int)((result.version_raw >> 8) & 0xFFUL);
    result.version_tertiary =
        (unsigned int)(result.version_raw & 0xFFUL);
    *capability = result;
    return W98_NVME_OK;
}
