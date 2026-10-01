# ShizukuDOS isolated SMP work prototype

This is a separate BIOS floppy experiment. It does **not** replace
`shizukudos/boot.asm` or `stage2.asm`, and it does not add multicore scheduling
to Windows 98 VMM, DOS, or applications. The BSP assigns one calculation to
each AP, performs its own calculation while the APs run, checks every result,
then all CPUs park. No operating-system task switch occurs. The default image
keeps the previously verified real-mode AP path; `--protected-ap` selects a
separate protected-mode AP image.

## Boot, work, and validation

1. `boot.asm` loads a 16-sector BSP binary at physical `0x8000` and a
   one-sector AP trampoline at physical `0x7000` from a 1.44 MB floppy.
2. `bsp.asm` enters 32-bit flat protected mode with paging off. It verifies
   CPUID local-APIC support and IA32_APIC_BASE, reads its APIC ID, and finds
   the ACPI RSDP, RSDT, and MADT. It checks RSDP/SDT checksums and bounded
   table lengths; only enabled xAPIC processor entries are accepted. Duplicate
   enabled xAPIC IDs, including a second BSP ID, are rejected before any IPI.
3. The BSP assigns one 12-byte shared-memory job slot per APIC ID at
   `0x6200 + 12 * ID`. Each slot contains status, seed, and result. The seed
   is `0x5A17C0DE xor ID`. Shared counters at `0x6000` track acknowledgement,
   start, completion, failure, and work progress. One AP progress byte per
   xAPIC ID lives at `0x6e00`; BSP observation state lives at `0x6f00`.
4. For each AP, the BSP sends INIT assert, INIT deassert, and two SIPIs via
   the xAPIC ICR. A PIT channel-2 one-shot supplies approximately 10 ms and
   200 us delays. ICR delivery and PIT polls are bounded.
5. SIPI vector `07h` enters `ap.asm` (or opt-in `ap_pm.asm`) at `0x7000`.
   Each AP discovers its
   initial APIC ID via CPUID, rejects a duplicate SIPI, acknowledges with
   `lock inc`, waits for its assigned job and the BSP work flag, and hashes
   its seed for 2,000,000 iterations. The BSP concurrently hashes its own
   seed for 16,000,000 iterations. In both programs each iteration computes
   `value = rol32(value, 5) xor 0x9E3779B9; value += iteration_index`, with
   32-bit wraparound.
6. The BSP publishes a progress value after each 16,384 actual hash iterations.
   Each AP publishes its own monotonic progress byte after each 32,768 actual
   iterations. An AP samples BSP progress after 500,000 iterations and again
   after 1,500,000; it counts overlap only if the BSP value increased while
   the BSP work flag remained set. During its own hash loop, the BSP also
   samples each AP progress byte at successive checkpoints and counts it only
   when it increases between observations. Both counts must equal the
   expected AP count. The BSP then independently recomputes every AP hash
   and checks its slot status/result. The host tests independently recompute
   the printed per-CPU results and check both progress counts.

The BSP uses a separate approximately 2-second PIT budget for each
acknowledgement/start/completion phase. AP waits have finite `pause` loops and
report a failure through an atomic counter and slot status. A fault-injection
build flips one completed AP result bit before BSP verification, exercising
the explicit mismatch failure path. Another build lets an AP finish its job
without incrementing `DONE_COUNT`, so the BSP must report a completion
timeout. Additional fault images make the BSP wait until AP completion after
setting the old work flag, suppress AP progress publication, or synthesize a
duplicate enabled MADT xAPIC ID. These exercise the two-sided progress gate
and early table rejection. The synthetic duplicate injects a repeated target
in the MADT parser; it does not rewrite firmware ACPI bytes. These are
bounded smoke-test waits, not a general-purpose synchronization or scheduling
API.

The prototype supports at most 32 APs with 8-bit xAPIC IDs. It rejects
enabled x2APIC MADT entries, malformed tables, and unavailable APIC/PIT
operations. It assumes BIOS/CSM and an ACPI RSDT below 4 GiB. In the default
image APs execute this one real-mode hash job and then park in `HLT`. In the
opt-in image, each AP transitions from SIPI real mode to flat 32-bit protected
mode, installs an immutable local GDT, derives a distinct 512-byte stack from
its initial 8-bit xAPIC ID, writes and reads a stack canary, then performs
the same job in protected mode. The stack range is `0x30000..0x50000`; the
BSP checks the BIOS conventional-memory field is at least 512 KiB before it
starts APs. APs increment a separate atomic protected-mode entry count, and
the BSP requires it to equal the expected AP count after work completes.
Both images leave interrupts disabled and paging off. There is no AP IDT,
task scheduler, interrupt routing, DOS contract, or Windows 98 VMM SMP path.
Those remain separate work before usable OS-level multicore support can be
claimed.

## Build and test

From the repository root, with NASM and Python 3:

```powershell
py -3 shizukudos/smp/build.py
py -3 shizukudos/smp/build.py --fault-inject --output shizukudos/smp/out/smp-fault.img
py -3 shizukudos/smp/build.py --omit-done --output shizukudos/smp/out/smp-omit-done.img
py -3 shizukudos/smp/test_qemu.py --accel tcg
py -3 shizukudos/smp/test_vbox.py
py -3 shizukudos/smp/build.py --protected-ap --output shizukudos/smp/out/smp-pm.img
py -3 shizukudos/smp/test_qemu.py --accel tcg --protected-ap
py -3 shizukudos/smp/test_vbox.py --protected-ap
```

The generated floppies, serial logs, and per-run evidence JSON stay in the
Git-ignored `smp/out/` directory. `test_qemu.py` boots with `-smp 2` and
`-smp 4`, verifies counters and every per-CPU result, disables ACPI to test
a specific bounded failure, and boots the result-corruption and missing-done
images to test validation and timeout. Its default accelerator is `whpx` on
Windows and `kvm` on Linux;
`--accel tcg` is explicitly a functional fallback. The QEMU binary installed
on this development host reports only `tcg` from `-accel help`, so QEMU runs
do not satisfy the project's mandatory VT-x/AMD-V gate.

`test_vbox.py` creates **new disposable VMs only** under `smp/out/`, with BIOS,
a floppy controller, and 2 or 4 CPUs. It checks the serial counters/results,
tests both failure images, and requires the *current* `VBox.log` to
identify `NEM: Created partition` (WHPX/NEM hardware backend) or a direct HM
VT-x/AMD-V backend. It then powers off and unregisters the disposable VMs.
It never opens the installed Windows 98 VM. A cleanup failure is reported
with the unique disposable VM name and path for inspection.

The `--protected-ap` test option repeats the 2- and 4-CPU normal work tests,
the missing-MADT test in QEMU, and the result-corruption and missing-done
tests on the protected-mode image. It additionally builds an image in which
an AP completes the hash but deliberately omits its protected-mode entry
marker; the BSP must report a specific entry-count mismatch. The test
requires the protected-mode banner, exact entry count, and published stack
layout. It does not infer protected-mode work from CPUID or CPU count alone.
Both test scripts also run the stalled-BSP, silent-AP, and duplicate-ID fault
images for the selected AP mode. The VirtualBox runner requires the two-sided
progress line before it evaluates a result, then checks the execution backend
from each disposable VM's *current* log.

## Evidence on this host, 2026-09-24

- Normal NASM image: 1,474,560 bytes, SHA-256
  `3be0986c4195af1b6f8781f68225a1eff93e1c3676c524f6832daa01e653d756`.
- Result-corruption image: 1,474,560 bytes, SHA-256
  `84f4d3557e017eabfe4ba7567f7f02c9888f9357131d78e110835b71c6bb1460`.
- Missing-completion image: 1,474,560 bytes, SHA-256
  `7a77c2572cd51986b9ca1842226fa8522e6810b95134ed44bd936a13e32ea711`.
- QEMU TCG, 2 CPUs: BSP APIC ID 0, one AP; acknowledgement, start,
  completion, and AP-side overlap all 1/1; BSP-side observed progress 1/1;
  independently checked per-CPU
  results; pass.
- QEMU TCG, 4 CPUs: BSP APIC ID 0, three APs; those four counts all 3/3;
  BSP-side observed progress 3/3; independently checked per-CPU results; pass.
- QEMU TCG with ACPI disabled: explicit missing-MADT error and bounded exit;
  pass. QEMU TCG with a corrupted AP result: explicit mismatch and bounded
  failure exit; pass. QEMU TCG with the AP completion signal omitted: explicit
  BSP completion timeout and bounded failure exit; pass.
- VirtualBox hardware-backed disposable VM, 2 CPUs: all four AP counts 1/1,
  BSP-side observed progress 1/1, per-CPU results verified, current log
  reported WHPX/NEM partition; pass.
- VirtualBox hardware-backed disposable VM, 4 CPUs: all four AP counts 3/3,
  BSP-side observed progress 3/3, per-CPU results verified, current log
  reported WHPX/NEM partition; pass.
- VirtualBox hardware-backed result-corruption run, 2 CPUs: all AP work
  counters 1/1 but BSP rejected the altered result, current log reported
  WHPX/NEM partition; pass.
- VirtualBox hardware-backed missing-completion run, 2 CPUs: the AP
  acknowledged, started, and overlapped BSP work, but the BSP reported a
  completion timeout; current log reported WHPX/NEM partition; pass.
  Disposable VMs were unregistered after testing.
- Stalled-BSP fault: the BSP set `BSP_RUNNING` but waited until the AP had
  finished before doing its own hash. QEMU and hardware-backed VirtualBox
  rejected it with AP-side overlap 0 and BSP-side observed progress 0.
- Silent-AP fault: the AP completed its work but suppressed progress-byte
  publication. Both runtimes rejected it despite AP-side overlap 1, because
  BSP-side observed progress stayed 0.
- Duplicate-MADT fault: both runtimes rejected the synthetic duplicate
  enabled target xAPIC ID before sending an IPI (acknowledgement 0).

These results establish AP startup, overlapping BSP/AP work intervals, and
checked shared-memory coordination in this isolated boot environment. They
do **not** establish Windows 98 VMM SMP scheduling or parallel application
execution. No Windows installation media, product key, or Microsoft binary
is in the test image.

## Protected-mode AP evidence on this host, 2026-09-24

- Opt-in normal image: 1,474,560 bytes, SHA-256
  `0640138ac764663a9359f1ccfc81fe62ada679deb8bcedc6871f6bcf30499a46`.
  The default real-mode image also changed because its progress gate was
  strengthened; its current SHA-256 is listed above.
- QEMU TCG, 2 CPUs: one AP entered protected mode; acknowledgement, start,
  completion, and AP-side overlap were 1/1; BSP-side observed progress 1/1;
  host independently recomputed
  each reported hash. QEMU TCG, 4 CPUs: three APs entered protected mode;
  all four counters and BSP-side observed progress were 3/3, with
  independently recomputed hashes.
- QEMU TCG negative paths: missing MADT, corrupted AP result, missing AP
  completion, and missing protected-mode entry marker all reached their
  specific bounded failure results. The last image let the AP complete real
  work, then the BSP rejected entry count 0 rather than accepting the hash.
- Disposable VirtualBox 2- and 4-CPU BIOS VMs passed the same work and
  protected-mode entry checks; their *current* VBox.log files recorded
  `NEM: Created partition` for the WHPX/NEM hardware backend. Disposable
  2-CPU corruption, completion omission, and PM-marker omission runs also
  reached their specific failure results under that backend.
- Both runtimes also rejected the protected-mode stalled-BSP, silent-AP, and
  duplicate-MADT images with the same specific counter checks. The synthetic
  duplicate reached the malformed-table result before any AP acknowledgement.
- The fault, missing-completion, and missing-entry image SHA-256 values were
  `57c0073dd64f5586daeceeaac706c949afc3989d9d8603789133b5974e6b2149`,
  `0da2c652a7d0a3e2fa3dd05f7baf32d55660f40199fa0342a4b7fe9c33fae81e`,
  and `5cba1fc9e0e18de15c100083563639d4701d7896d70fd42127b8a87fdf0a2301`
  respectively. Per-run serial transcripts and backend lines remain in the
  Git-ignored `smp/out/` evidence JSON files.

This proves a protected-mode AP computation in an isolated BIOS floppy. It
does **not** show that installed Windows 98 uses any additional CPU or that
the VMM scheduler, interrupt model, or memory manager has been changed.

## Specification references

- [Intel Software Developer's Manual, multiprocessor startup sequence](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366821.pdf): INIT, the 10 ms delay, and two SIPIs separated by about 200 us.
- [ACPI 6.6 software programming model](https://uefi.org/specs/ACPI/6.6/05_ACPI_Software_Programming_Model.html): RSDP checksum, RSDT, MADT processor Local APIC records, IDs, and enabled flags.
- [VirtualBox 7.2 user guide](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/EN-VBOX-7-2-USER.pdf): `VBox.log` as the per-run source for execution-backend evidence.
