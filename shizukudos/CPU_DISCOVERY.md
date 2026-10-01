# ShizukuDOS CPU discovery foundation

The real-mode shell now collects a **read-only** CPU snapshot at boot and
prints it with `CPU`. Its assembler is independent project code under
GPL-2.0-only. The 16-bit shell remains a single-CPU DOS-like environment.

## Method

- On a 386-compatible entry path, toggle EFLAGS.AC to distinguish an i386
  from an i486-class processor, restore EFLAGS, then toggle EFLAGS.ID before
  executing any `CPUID`. If ID cannot change, no `CPUID` opcode is executed.
  This follows the processor feature-detection procedure in the
  [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
- With `CPUID`, leaf 0 bounds subsequent basic-leaf reads. Leaf 1 supplies
  vendor/family/model/stepping, the package logical count where advertised,
  PAE, XSAVE, OSXSAVE, and AVX bits. Extended family and model fields are
  decoded only for the specified base families.
- Package topology prefers Intel's V2 leaf `1Fh`, then leaf `0Bh`. Each valid
  level is bounded to eight subleaves; the highest valid level contributes
  the package logical count, while the SMT level contributes threads per
  core. A valid Core level and an integral logical/thread split are required.
  Intel [recommends leaf `1Fh` ahead of `0Bh`](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
- When extended topology is unavailable, deterministic-cache leaf 4 is a
  bounded Intel fallback; AMD vendor leaf `80000008h` supplies a core count
  when present. Inconsistent counts leave cores/threads as `unknown` rather
  than claiming a false topology. AMD's extended CPUID layout is documented
  in its [CPUID specification](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/design-guides/25481.pdf)
  and [Architecture Programmer's Manual](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2).
- `AVX HW` means the leaf 1 silicon feature bit is set. `AVX usable now`
  requires XSAVE, OSXSAVE, and AVX bits, clear CR0.EM/TS, **and**
  `XGETBV(XCR0)` bits 1 and 2.
  `XGETBV` is never executed without those three prerequisite bits. This
  shell does not enable CR4.OSXSAVE or XCR0, so normal real-mode boot reports
  AVX hardware as present but unusable.

The source is `cpu_detect.inc`, included in `stage2.asm`. It uses only
386-era 32-bit general-register instructions before testing CPUID; it does
not require protected mode or a C runtime. The boot image remains below the
31-sector reserved-stage limit.

## Verification performed

`./shizukudos/build.ps1` builds the FAT12 image.
`python ./shizukudos/tests/run_cpu_probe.py` boots ephemeral QEMU instances
with a read-only copy of that image, sends `CPU` through COM1, and checks the
observed output. It passed these eight cases:

- `486` and `486,level=0`: family 4 decoding and a missing-leaf-1 guard.
- `pentium`: family 5 and absent PAE/XSAVE/AVX bits.
- `qemu32` with one and two vCPUs: package count and leaf-4 core fallback.
- AMD `phenom` with two vCPUs: AMD extended-leaf core fallback.
- `SandyBridge` with two cores; then four logical vCPUs as two cores with
  two SMT threads each: leaf-0Bh split, and AVX hardware present while
  OSXSAVE and current AVX eligibility stay off.

QEMU's available `486` model **does expose CPUID**, so neither a true 386
nor a no-CPUID 486 executed the EFLAGS.ID rejection branch in this test.
The `486,level=0` case tests a CPUID-capable CPU with no leaf 1; it is not a
substitute for that hardware branch. These QEMU TCG runs are development
checks, not the required VT-x/AMD-V accelerated acceptance path. The
highest-level `1Fh` hierarchy and AVX-active path also await a suitable
emulator/guest fixture.

## Scope boundary

The counts are **CPUID hints for the current package**, not processors
enumerated, started, or scheduled by ShizukuDOS or Windows 98. No AP startup,
local APIC setup, interprocessor interrupts, interrupt routing, locks,
multicore scheduler, or Windows 98 VMM change is included. `PAE HW Y` is only
the hardware capability bit; no PAE page tables or additional RAM are in use.
Per-app i386/i486/Pentium/Modern instruction and speed profiles are also not
enforced by this discovery command.
