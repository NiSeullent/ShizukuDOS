#include "w98_hw_probe.h"
#include <stdio.h>

struct fake_regs {
    w98_u32 regs[12];
    unsigned int reads;
};

static w98_u32 fake_read32(void *context, w98_u32 offset)
{
    struct fake_regs *regs = (struct fake_regs *)context;
    ++regs->reads;
    if ((offset & 3UL) != 0UL || offset >= sizeof(regs->regs))
        return 0UL;
    return regs->regs[offset / 4UL];
}

static int check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "failed: %s\n", name);
        return 0;
    }
    return 1;
}

int main(void)
{
    struct fake_regs regs = {{0}, 0};
    struct w98_ahci_info ahci;
    struct w98_xhci_info xhci;
    int passed = 1;

    passed &= check(w98_probe_ahci(fake_read32, &regs, 0x20UL, &ahci) ==
                    W98_PROBE_REGION_TOO_SMALL && regs.reads == 0U,
                    "AHCI short region never reads MMIO");

    regs.regs[0x00 / 4] = 0x80001F05UL;
    regs.regs[0x0C / 4] = 0x3FUL;
    regs.regs[0x10 / 4] = 0x00010301UL;
    regs.regs[0x24 / 4] = 1UL;
    regs.regs[0x28 / 4] = 1UL;
    passed &= check(w98_probe_ahci(fake_read32, &regs, 0x2CUL, &ahci) ==
                    W98_PROBE_OK && ahci.port_count == 6U &&
                    ahci.command_slots == 32U &&
                    ahci.implemented_ports == 0x3FUL &&
                    ahci.supports_64bit_dma == 1U &&
                    ahci.supports_bios_handoff == 1U,
                    "AHCI capability decoding");

    regs.regs[0x00 / 4] = 0x0000001FUL;
    regs.regs[0x0C / 4] = 0xFFFFFFFFUL;
    regs.regs[0x24 / 4] = 0UL;
    passed &= check(w98_probe_ahci(fake_read32, &regs, 0x2CUL, &ahci) ==
                    W98_PROBE_OK && ahci.port_count == 32U &&
                    ahci.implemented_ports == 0xFFFFFFFFUL &&
                    ahci.supports_bios_handoff == 0U && ahci.bohc == 0UL,
                    "AHCI 32-port mask and absent handoff");

    regs.reads = 0U;
    passed &= check(w98_probe_xhci(fake_read32, &regs, 0x10UL, &xhci) ==
                    W98_PROBE_REGION_TOO_SMALL && regs.reads == 0U,
                    "xHCI short region never reads MMIO");

    regs.regs[0x00 / 4] = 0x01000020UL;
    regs.regs[0x04 / 4] = (6UL << 24) | (1UL << 8) | 32UL;
    regs.regs[0x10 / 4] = 1UL;
    regs.regs[0x14 / 4] = 0x1000UL;
    regs.regs[0x18 / 4] = 0x2000UL;
    passed &= check(w98_probe_xhci(fake_read32, &regs, 0x3000UL, &xhci) ==
                    W98_PROBE_OK && xhci.cap_length == 0x20U &&
                    xhci.version == 0x0100U && xhci.max_slots == 32U &&
                    xhci.max_interrupters == 1U && xhci.max_ports == 6U &&
                    xhci.doorbell_offset == 0x1000UL &&
                    xhci.runtime_offset == 0x2000UL,
                    "xHCI capability decoding");
    passed &= check(w98_probe_xhci(fake_read32, &regs, 0x2010UL, &xhci) ==
                    W98_PROBE_OFFSET_OUT_OF_RANGE,
                    "xHCI runtime window bounds");

    if (passed)
        puts("probe tests passed");
    return passed ? 0 : 1;
}
