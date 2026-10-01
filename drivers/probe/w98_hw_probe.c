#include "w98_hw_probe.h"

/* Intel AHCI 1.3.1, generic host registers (§3.1). */
#define AHCI_CAP       0x00UL
#define AHCI_GHC       0x04UL
#define AHCI_PI        0x0CUL
#define AHCI_VS        0x10UL
#define AHCI_CAP2      0x24UL
#define AHCI_BOHC      0x28UL
#define AHCI_MIN_BYTES 0x2CUL

/* Intel xHCI, capability registers (§5.3). */
#define XHCI_CAP0       0x00UL
#define XHCI_HCSPARAMS1 0x04UL
#define XHCI_HCCPARAMS1 0x10UL
#define XHCI_DBOFF      0x14UL
#define XHCI_RTSOFF     0x18UL
#define XHCI_MIN_BYTES  0x1CUL

static int range_fits(w98_u32 offset, w98_u32 bytes, w98_u32 length)
{
    return offset <= length && bytes <= length - offset;
}

int w98_probe_ahci(w98_mmio_read32 read32, void *context,
                   w98_u32 mapped_length, struct w98_ahci_info *info)
{
    w98_u32 mask;

    if (read32 == 0 || info == 0)
        return W98_PROBE_BAD_ARGUMENT;
    if (mapped_length < AHCI_MIN_BYTES)
        return W98_PROBE_REGION_TOO_SMALL;

    info->cap = read32(context, AHCI_CAP);
    info->ghc = read32(context, AHCI_GHC);
    info->pi = read32(context, AHCI_PI);
    info->version = read32(context, AHCI_VS);
    info->cap2 = read32(context, AHCI_CAP2);
    /* BOHC exists only when CAP2.BOH advertises the handoff mechanism. */
    info->bohc = (info->cap2 & 1UL) ? read32(context, AHCI_BOHC) : 0UL;

    info->port_count = (unsigned int)((info->cap & 0x1FUL) + 1UL);
    info->command_slots = (unsigned int)(((info->cap >> 8) & 0x1FUL) + 1UL);
    info->supports_64bit_dma = (unsigned int)((info->cap >> 31) & 1UL);
    info->supports_bios_handoff = (unsigned int)(info->cap2 & 1UL);
    mask = (info->port_count == 32) ? 0xFFFFFFFFUL :
           ((1UL << info->port_count) - 1UL);
    info->implemented_ports = info->pi & mask;

    return W98_PROBE_OK;
}

int w98_probe_xhci(w98_mmio_read32 read32, void *context,
                   w98_u32 mapped_length, struct w98_xhci_info *info)
{
    w98_u32 doorbell_raw;
    w98_u32 runtime_raw;

    if (read32 == 0 || info == 0)
        return W98_PROBE_BAD_ARGUMENT;
    if (mapped_length < XHCI_MIN_BYTES)
        return W98_PROBE_REGION_TOO_SMALL;

    info->cap0 = read32(context, XHCI_CAP0);
    info->hcsparams1 = read32(context, XHCI_HCSPARAMS1);
    info->hccparams1 = read32(context, XHCI_HCCPARAMS1);
    doorbell_raw = read32(context, XHCI_DBOFF);
    runtime_raw = read32(context, XHCI_RTSOFF);

    info->cap_length = (unsigned int)(info->cap0 & 0xFFUL);
    info->version = (unsigned int)((info->cap0 >> 16) & 0xFFFFUL);
    info->max_slots = (unsigned int)(info->hcsparams1 & 0xFFUL);
    info->max_interrupters = (unsigned int)((info->hcsparams1 >> 8) & 0x7FFUL);
    info->max_ports = (unsigned int)((info->hcsparams1 >> 24) & 0xFFUL);
    info->supports_64bit_dma = (unsigned int)(info->hccparams1 & 1UL);
    info->doorbell_offset = doorbell_raw & ~3UL;
    info->runtime_offset = runtime_raw & ~31UL;

    if (info->cap_length < 0x20U || (info->cap_length & 3U) != 0U ||
        info->max_slots == 0U || info->max_interrupters == 0U ||
        info->max_ports == 0U)
        return W98_PROBE_BAD_CAPABILITY;

    /* Verify the first operational register, doorbell and interrupter 0. */
    if (!range_fits((w98_u32)info->cap_length, 4UL, mapped_length) ||
        !range_fits(info->doorbell_offset, 4UL, mapped_length) ||
        !range_fits(info->runtime_offset, 0x40UL, mapped_length))
        return W98_PROBE_OFFSET_OUT_OF_RANGE;

    return W98_PROBE_OK;
}
