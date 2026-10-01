#include "nvme_probe.h"
#include <stdio.h>
#include <string.h>

struct fake_device {
    w98_nvme_u32 config[6];
    w98_nvme_u32 registers[3];
    unsigned int devices;
    unsigned int find_calls;
    unsigned int config_reads;
    unsigned int mmio_reads;
    unsigned int unexpected_offsets;
    unsigned int seen_offsets[3];
    int find_error;
    int invalid_bdf;
    int fail_config_offset;
    int fail_mmio_offset;
};

static void setup(struct fake_device *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->devices = 2U;
    fake->config[0] = 0x56781234UL;
    fake->config[1] = 0x00000002UL;
    fake->config[2] = 0x010802A1UL;
    fake->config[3] = 0x00000000UL;
    fake->config[4] = 0xFEC00004UL;
    fake->config[5] = 0UL;
    fake->registers[0] = (10UL << 24) | 63UL;
    fake->registers[1] = (2UL << 20) | 3UL;
    fake->registers[2] = 0x00010400UL;
    fake->fail_config_offset = -1;
    fake->fail_mmio_offset = -1;
}

static int find_class(void *context, w98_nvme_u32 class_code,
                      unsigned int index, struct w98_nvme_bdf *bdf)
{
    struct fake_device *fake = (struct fake_device *)context;
    ++fake->find_calls;
    if (class_code != W98_NVME_PCI_CLASS_CODE) return -1;
    if (fake->find_error) return -1;
    if (index >= fake->devices) return 0;
    bdf->bus = 2U;
    bdf->device = fake->invalid_bdf ? 32U : (unsigned char)(5U + index);
    bdf->function = 0U;
    return 1;
}

static int config_read32(void *context, const struct w98_nvme_bdf *bdf,
                         unsigned int offset, w98_nvme_u32 *value)
{
    struct fake_device *fake = (struct fake_device *)context;
    ++fake->config_reads;
    if (bdf->bus != 2U || bdf->device < 5U || bdf->device > 6U ||
        bdf->function != 0U || offset >= 24U || (offset & 3U) != 0U) {
        ++fake->unexpected_offsets;
        return 0;
    }
    if ((int)offset == fake->fail_config_offset) return 0;
    *value = fake->config[offset / 4U];
    return 1;
}

static int mmio_read32(void *context, w98_nvme_u32 offset,
                       w98_nvme_u32 *value)
{
    struct fake_device *fake = (struct fake_device *)context;
    unsigned int slot;
    ++fake->mmio_reads;
    if (offset > 8UL || (offset & 3UL) != 0UL) {
        ++fake->unexpected_offsets;
        return 0;
    }
    slot = (unsigned int)(offset / 4UL);
    ++fake->seen_offsets[slot];
    if ((int)offset == fake->fail_mmio_offset) return 0;
    *value = fake->registers[slot];
    return 1;
}

static int expect(int condition, const char *name)
{
    if (!condition) fprintf(stderr, "FAIL: %s\n", name);
    return condition;
}

int main(void)
{
    struct fake_device fake;
    struct w98_nvme_pci_info pci;
    struct w98_nvme_pci_info untouched_pci;
    struct w98_nvme_capability cap;
    struct w98_nvme_capability untouched_cap;
    int passed = 1;

    setup(&fake);
    memset(&pci, 0xA5, sizeof(pci));
    untouched_pci = pci;
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, W98_NVME_MAX_CLASS_INDEX, &pci) ==
                    W98_NVME_BAD_ARGUMENT && fake.find_calls == 0U &&
                    memcmp(&pci, &untouched_pci, sizeof(pci)) == 0,
                    "bounded PCI class index");
    passed &= expect(w98_nvme_discover_index(NULL, config_read32,
                    &fake, 0U, &pci) == W98_NVME_BAD_ARGUMENT &&
                    fake.find_calls == 0U, "missing PCI callback");

    fake.devices = 0U;
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, 0U, &pci) == W98_NVME_NOT_FOUND &&
                    fake.config_reads == 0U &&
                    memcmp(&pci, &untouched_pci, sizeof(pci)) == 0,
                    "no PCI device leaves output alone");

    setup(&fake);
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, 1U, &pci) == W98_NVME_OK &&
                    pci.bdf.bus == 2U && pci.bdf.device == 6U &&
                    pci.vendor_id == 0x1234U && pci.device_id == 0x5678U &&
                    pci.revision_id == 0xA1U && pci.memory_space_enabled &&
                    pci.bar_is_64bit && pci.bar_base_low == 0xFEC00000UL &&
                    pci.bar_base_high == 0UL && fake.config_reads == 6U &&
                    fake.unexpected_offsets == 0U,
                    "PCI class, identity, command and BAR0/1 decode");

    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    11UL, &cap) == W98_NVME_REGION_TOO_SMALL &&
                    fake.mmio_reads == 0U, "short region performs no read");
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_OK &&
                    cap.cap_low == fake.registers[0] &&
                    cap.cap_high == fake.registers[1] &&
                    cap.version_raw == fake.registers[2] &&
                    cap.max_queue_entries == 64UL &&
                    cap.timeout_500ms_units == 10U &&
                    cap.doorbell_stride_shift == 3U &&
                    cap.min_memory_page_shift == 12U &&
                    cap.max_memory_page_shift == 14U &&
                    cap.version_major == 1U && cap.version_minor == 4U &&
                    cap.version_tertiary == 0U && fake.mmio_reads == 3U &&
                    fake.seen_offsets[0] == 1U &&
                    fake.seen_offsets[1] == 1U &&
                    fake.seen_offsets[2] == 1U &&
                    fake.unexpected_offsets == 0U,
                    "CAP and VS decode with exactly three in-bounds reads");

    memset(&cap, 0xA5, sizeof(cap));
    untouched_cap = cap;
    fake.mmio_reads = 0U;
    pci.memory_space_enabled = 0U;
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_MEMORY_DISABLED &&
                    fake.mmio_reads == 0U &&
                    memcmp(&cap, &untouched_cap, sizeof(cap)) == 0,
                    "disabled PCI memory prevents MMIO");
    pci.memory_space_enabled = 1U;
    pci.bar_base_high = 1UL;
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_BAR_ABOVE_4G &&
                    fake.mmio_reads == 0U,
                    "above-4G BAR waits for physical mapper");
    pci.bar_base_high = 0UL;
    pci.bar_base_low = 0UL;
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_BAD_BAR &&
                    fake.mmio_reads == 0U &&
                    memcmp(&cap, &untouched_cap, sizeof(cap)) == 0,
                    "unassigned BAR prevents MMIO");
    pci.bar_base_low = 0xFEC00000UL;
    pci.bar_is_64bit = 0U;
    pci.bar_base_high = 1UL;
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_BAD_BAR &&
                    fake.mmio_reads == 0U,
                    "inconsistent 32-bit BAR rejected");
    pci.bar_base_high = 0UL;
    pci.bar_is_64bit = 1U;
    passed &= expect(w98_nvme_probe_mmio(&pci, NULL, &fake,
                    12UL, &cap) == W98_NVME_BAD_ARGUMENT &&
                    fake.mmio_reads == 0U,
                    "missing MMIO callback prevents access");
    fake.fail_mmio_offset = 4;
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_MMIO_ERROR &&
                    fake.mmio_reads == 2U &&
                    memcmp(&cap, &untouched_cap, sizeof(cap)) == 0,
                    "MMIO read failure does not publish partial results");
    fake.fail_mmio_offset = -1;
    fake.registers[0] = 0UL;
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_BAD_CAPABILITY &&
                    memcmp(&cap, &untouched_cap, sizeof(cap)) == 0,
                    "zero MQES rejected");
    fake.registers[0] = 1UL;
    fake.registers[1] = (1UL << 16);
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_BAD_CAPABILITY,
                    "MPSMIN above MPSMAX rejected");
    fake.registers[1] = 0UL;
    fake.registers[2] = 0UL;
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_BAD_CAPABILITY,
                    "zero VS rejected");
    fake.registers[2] = 0xFFFF0000UL;
    passed &= expect(w98_nvme_probe_mmio(&pci, mmio_read32, &fake,
                    12UL, &cap) == W98_NVME_BAD_CAPABILITY &&
                    memcmp(&cap, &untouched_cap, sizeof(cap)) == 0,
                    "all-ones major version rejected");

    setup(&fake);
    memset(&pci, 0xA5, sizeof(pci));
    untouched_pci = pci;
    fake.config[2] = 0x01060100UL;
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, 0U, &pci) == W98_NVME_BAD_CLASS &&
                    memcmp(&pci, &untouched_pci, sizeof(pci)) == 0,
                    "mismatched BIOS class rejected");
    fake.config[2] = 0x010802A1UL;
    fake.config[4] = 0xFEC00001UL;
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, 0U, &pci) == W98_NVME_BAD_BAR,
                    "I/O BAR rejected");
    fake.config[4] = 0UL;
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, 0U, &pci) == W98_NVME_BAD_BAR,
                    "unassigned BAR rejected");
    fake.config[4] = 0xFEC00000UL;
    fake.config[3] = 0x00010000UL;
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, 0U, &pci) == W98_NVME_BAD_BAR,
                    "non-endpoint PCI header rejected");
    fake.config[3] = 0UL;
    fake.fail_config_offset = 8;
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, 0U, &pci) == W98_NVME_PCI_ERROR,
                    "config read failure reported");
    fake.fail_config_offset = -1;
    fake.invalid_bdf = 1;
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, 0U, &pci) == W98_NVME_PCI_ERROR,
                    "invalid BDF rejected before config access");

    setup(&fake);
    fake.config[4] = 0xFEC00000UL;
    passed &= expect(w98_nvme_discover_index(find_class, config_read32,
                    &fake, 0U, &pci) == W98_NVME_OK &&
                    !pci.bar_is_64bit && fake.config_reads == 5U,
                    "32-bit BAR does not read BAR1");

    if (passed) puts("PASS: read-only NVMe PCI discovery and MMIO probe");
    return passed ? 0 : 1;
}
