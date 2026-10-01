# ShizukuDOS isolated PAE high-memory experiment

This directory builds a **separate BIOS floppy**, not a patch to the existing
ShizukuDOS boot chain or Windows 98. The probe enters 32-bit PAE paging, reads
and writes one dword at physical **`0x0000000100001000` (4 GiB + 4 KiB)**,
restores the original value and returns to nonpaged protected mode. It does
not demonstrate Windows 98 VMM PAE support, guest GUI boot above 4 GiB, or
usable high memory across all pages.

## Mechanism and checks

1. `boot.asm` loads `stage.asm` from a 1.44 MB BIOS floppy. In real mode,
   `stage.asm` calls INT 15h E820. It requires one type-1 usable RAM
   descriptor covering the complete 2 MiB page `[4 GiB, 4 GiB + 2 MiB)`;
   otherwise it reports a bounded failure without entering PAE. It then
   enables A20 and enters 32-bit flat protected mode.
2. After checking `CPUID.01H:EDX.PAE`, it saves CR0, CR3 and CR4. Four PAE
   page directories identity-map the low 4 GiB with 2 MiB pages. The first
   2 MiB slot of virtual `0xC0000000` is redirected to physical 4 GiB by
   setting the high dword of that 64-bit PDE to `1`.
3. It sets CR4.PAE, loads the aligned PDPT in CR3, enables CR0.PG and
   checks those bits and the PDE. Virtual `0xC0001000` then names physical
   `0x100001000`. The original dword is read, an always-different pattern
   (`original XOR 0xA55A96C3`) is written and read back. Physical `0x1000`
   is read before and during the write as an alias guard against truncating
   the high address to 32 bits. The original high dword is restored and read
   back. Normal and comparison-failure paths turn paging off before restoring
   CR3 and CR4. A minimal #GP/#PF handler also turns paging off and reports
   an error; an unexpected fault after the high write cannot promise data
   restoration, so this remains an isolated disposable-VM experiment.
4. Serial output reports the E820 upper bound, active paging bits, original
   value, pattern, observed value, alias-guard values and restored value.
   The host tests parse and cross-check all of those measurements; a printed
   `PASS` token alone is insufficient.

The choice of a 2 MiB page and 4-entry PDPT follows the
[Intel Software Developer's Manual](https://cdrdv2-public.intel.com/819714/253668-sdm-vol-3a.pdf).
The real-mode E820 descriptor layout and type-1 meaning follow the
[ACPI E820 specification](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/15_System_Address_Map_Interfaces/int-15h-e820h---query-system-address-map.html).

## Build and run

From the repository root with NASM, Python 3, QEMU and VirtualBox:

```powershell
py -3 shizukudos/pae/build.py
py -3 shizukudos/pae/test_qemu.py --accel tcg
py -3 shizukudos/pae/test_vbox.py
```

The QEMU test uses `-m 5120` for the high-page proof and `-m 3072` for the
negative E820 path. On this host the installed QEMU binary lists **TCG only**;
TCG is software emulation and does not meet the project's mandatory
VT-x/AMD-V requirement. The script defaults to WHPX on Windows or KVM on
Linux when those accelerators are available, and never silently falls back.

The VirtualBox test creates new disposable 5120 MiB and 3072 MiB BIOS VMs
under `pae/out/`, enables PAE and hardware virtualization, and attaches only
this floppy. It checks the *current* `VBox.log` for the WHPX/NEM partition
or a direct HM VT-x/AMD-V backend. It writes a local JSON evidence file with
the exact backend line and full serial transcript, then powers off and
unregisters each disposable VM. It never opens or changes the installed
Windows 98 VM. Generated images, logs and evidence stay Git-ignored in
`pae/out/`; no Windows binaries or product key are involved.

## Results on this host, 2026-09-24

- Host: VirtualBox `7.2.18r175117`; the installed QEMU has only TCG.
  Approximately 16 GiB of physical RAM was free before running one 5 GiB
  disposable VM at a time.
- Final floppy: 1,474,560 bytes; SHA-256
  `3aad7bccf00d4c905bae303d36105fe395229eba8309f6b750aad8716730087d`.
- QEMU TCG, 5120 MiB: high physical address read/write/restore and low-alias
  check passed. QEMU TCG, 3072 MiB: E820 correctly rejected the high page.
- VirtualBox, 5120 MiB: same high physical address proof passed, and the
  current execution log reported `NEM: Created partition` (WHPX hardware
  backend). VirtualBox, 3072 MiB: E820 rejection passed with the same
  hardware backend.
- One 5120 MiB hardware run reported E820 usable end `0x160000000`,
  original `0x00000000`, pattern/readback `0xA55A96C3`, low alias guard
  `0x00000000` both before and during the write, and restored high value
  `0x00000000`. The exact transcript and current-log line are retained in a
  `*-evidence.json` file under `pae/out/`. The disposable VMs were
  unregistered after testing.

The result proves that **this isolated 32-bit boot probe** accessed one
E820-advertised physical dword above 4 GiB through PAE on a hardware-backed
VirtualBox VM. A Windows 98 VMM PAE implementation would require separate
page-table, physical allocator, DMA/MMIO and driver work, followed by direct
installed-guest tests.
