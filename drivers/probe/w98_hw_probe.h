#ifndef W98_HW_PROBE_H
#define W98_HW_PROBE_H

/*
 * C89, read-only register decoding for 32-bit Windows 98 bring-up tools.
 * This is not a driver. The caller must safely map MMIO and provide read32.
 * Do not use a virtual pointer as a DMA/physical address.
 */

typedef unsigned long w98_u32;
typedef char w98_u32_is_four_bytes[(sizeof(w98_u32) == 4) ? 1 : -1];

typedef w98_u32 (*w98_mmio_read32)(void *context, w98_u32 byte_offset);

enum w98_probe_status {
    W98_PROBE_OK = 0,
    W98_PROBE_BAD_ARGUMENT = -1,
    W98_PROBE_REGION_TOO_SMALL = -2,
    W98_PROBE_BAD_CAPABILITY = -3,
    W98_PROBE_OFFSET_OUT_OF_RANGE = -4
};

struct w98_ahci_info {
    w98_u32 cap;
    w98_u32 ghc;
    w98_u32 pi;
    w98_u32 version;
    w98_u32 cap2;
    w98_u32 bohc;
    w98_u32 implemented_ports;
    unsigned int port_count;
    unsigned int command_slots;
    unsigned int supports_64bit_dma;
    unsigned int supports_bios_handoff;
};

struct w98_xhci_info {
    w98_u32 cap0;
    w98_u32 hcsparams1;
    w98_u32 hccparams1;
    w98_u32 doorbell_offset;
    w98_u32 runtime_offset;
    unsigned int cap_length;
    unsigned int version;
    unsigned int max_slots;
    unsigned int max_interrupters;
    unsigned int max_ports;
    unsigned int supports_64bit_dma;
};

int w98_probe_ahci(w98_mmio_read32 read32, void *context,
                   w98_u32 mapped_length, struct w98_ahci_info *info);
int w98_probe_xhci(w98_mmio_read32 read32, void *context,
                   w98_u32 mapped_length, struct w98_xhci_info *info);

#endif
