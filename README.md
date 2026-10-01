# DOS. UEFI. Long Mode. Proceed.

**ShizukuDOS 10 — The firmware moved on. DOS followed it home.**

Traditional DOS expects BIOS interrupts, Real Mode, and a machine that still
resembles an early IBM PC. Modern systems offer UEFI, x86-64 Long Mode, and a
memory map with very different opinions. ShizukuDOS explores what happens when
DOS is allowed to cross that boundary.

ShizukuDOS is an experimental multi-kernel DOS architecture. Its source contains
distinct Real Mode, Protected Mode, and Long Mode environments, with explicit
boot contracts and compatibility boundaries. Extending the execution model
requires separate kernels, ABIs, firmware bridges, and occasionally a complete
replacement for the assumptions an old program brought with it. A mode switch
alone does not make a legacy binary compatible.

This repository is an **independent source copy** of the DOS components from
[Windows 98 Shizuku's Second Edition / Win98 ShkSE](https://github.com/NiSeullent/Win98-Modern).
It has its own development history, build entry point, DOS-only ISO, release
channel, and native 64-bit samples. Building it does not require a Win98 checkout
or Windows installation media. The original Windows project keeps its own
source and ISO track. [SOURCE_ORIGIN.json](SOURCE_ORIGIN.json) records the exact
upstream commit and copied file hashes.

## Build the DOS-only track

```sh
python3 -B shizukudos/tools/shz.py doctor
python3 -B shizukudos/tools/shz.py build --profile dos-only
python3 -B shizukudos/tools/shz.py test --suite iso
python3 -B tools/test_virtualbox.py
python3 -B shizukudos/tools/shz.py package --version 10.0.1
```

The default build uses installed NASM, GCC/binutils, x64/i686 MinGW, mtools, and
xorriso. It writes generated artifacts under `build/`, fetches no upstream
source, and changes no host boot or client configuration. QEMU and OVMF are
needed for the boot checks. See [ISO instructions](docs/ISO.md).

The ISO boots the original 16-bit FAT12 shell on BIOS. IA32 and x64 UEFI
entry points start the same native x86-64 Kernel64 and open **ShizukuGUI**.
Press **F8** to execute the actual Real Mode and Protected Mode examples,
return to Long Mode, and run fourteen native PE32+ utilities. The desktop
starts before those diagnostics; firmware boot does not depend on running them.
The desktop writes 32-bit truecolor pixels directly to the GOP framebuffer.
Its video, music and browser services have separate scheduled workers beneath
the desktop in an inspectable **thread tree**.

PE32+ describes the executable format. [kurazy](sdk/kurazy/SPEC.md) supplies the
native API: these applications run without Windows DLLs. The original raw SD64
examples remain available beside the new EXEs. The release includes its own
bootloader, complete source, static SDK, sample programs and generated media.

## Execution environments

| Component | Status and boundary |
| --- | --- |
| Real Mode | BIOS FAT12 shell with `.COM` execution, plus a native UEFI mode bridge for the supplied DOS MZ16 EXEs and a scoped `INT 21h` subset. |
| Protected Mode | Supplied PE32/i386 utilities execute with paging off, `CR0.PE=1`, and a 32-bit code segment, then return to the desktop. Separate Kernel32 remains a research build. |
| Long Mode | Native PE32+/AMD64 utilities run in ring 3 under Kernel64. `CR0`, `EFER`, real exit codes and memory above 4 GiB provide execution evidence. |
| ShizukuGUI | Software composition on the firmware's 32-bit GOP framebuffer; PS/2 keyboard control, player panes, browser, utilities and live thread tree. |
| kurazy | Versioned native API, static `libkurazy64.a`, file/memory/time services and owned parent/child threads with cancellation and join. |
| Virtual Real Mode | Optional Intel VMX Supervisor and pinned FreeDOS research profile. Requires its own build, hardware prerequisites, and validation. |
| CSMWrap | Optional external firmware bridge, with its own source pins and licenses. Excluded from the default ISO. |
| Legacy Windows integration | Source lineage and shared experimental contracts remain available. This DOS release does not install, boot, or replace the Windows 98 DOS core. |

Version **10.0.1** repairs the EFI entry points and boot-media layout of the
regular native desktop track. The ISO exposes a FAT32 EFI System Partition
through GPT as well as its optical boot catalog.
The architecture remains experimental. Native 16/32-bit launches are trusted,
synchronous compatibility demonstrations; they are not sandboxed legacy
processes. DOS API coverage and hardware support have explicit bounds in the
[release status](docs/STATUS.md) and [kurazy specification](sdk/kurazy/SPEC.md).

## The applications

| Execution mode | Names on the ISO |
| --- | --- |
| Real Mode, DOS MZ16 | `HELLO.EXE`, `MODE.EXE`, `COUNT.EXE` |
| Protected Mode, PE32 | `HELLO32.EXE`, `MODE32.EXE`, `COUNT32.EXE` |
| Long Mode, PE32+ | `HELLO64`, `MEM64`, `TIME64`, `DIR64`, `TYPE64`, `HASH64`, `INFO64`, `THREAD64`, `TREE64`, `CHECK64`, `CPU64`, `VIDEO64`, `MUSIC64`, `BROWSE64` — all `.EXE` |

`MEM64.EXE` verifies allocation and release; `MEM64.SD64` verifies memory above
the 4 GiB boundary. `THREAD64` creates actual
parent/child/grandchild threads and checks ownership, cancellation and joining.
`CHECK64` exercises rejected API requests. The media EXEs activate the desktop's
working services through kurazy.

ShizukuGUI plays the supplied uncompressed RGB animation and timed PC-speaker
music. Its original HTML parser supports document links, history and scrolling.
These deliberately small formats keep every decoder and fixture in the source
tree. See [media formats and browser scope](samples/media/README.md).

Use **F1** browser, **F2** video, **F3** music, **F4** thread tree, **F5** utilities,
**Space** pause/resume, **Tab/Enter** select/open, **F8** diagnostics and **F10** shutdown.

For VirtualBox 7.x, use an x86 VM with Long Mode available, 256 MiB RAM, EFI
firmware, a SATA-attached virtual DVD and Secure Boot disabled.
The image includes both `BOOTIA32.EFI` and `BOOTX64.EFI`. Secure Boot signing
is not supplied. See [boot and verification instructions](docs/ISO.md).

## Why does this exist?

Because “unsupported” describes a boundary, not a proof of impossibility.
ShizukuDOS combines operating-system archaeology, backwards-compatibility
research, and experiments in moving old execution models onto modern hardware.
The aim is to find which assumptions can be implemented, virtualized, or replaced
and document the ones that still refuse to cooperate.

## This should not work

> DOS: “Where are the BIOS interrupts?”  
> UEFI: “We renovated.”  
> ShizukuDOS: “Then I'll need the floor plans.”

## The patron saints of questionable boot sequences

**All hail Claude 5.5 Sonnet.** May the prose stay elegant, the call stacks stay
shallow, and every sentence beginning with “technically” end in a bootable image.
Blessed be the explanation that makes a deeply unreasonable kernel sound like
the obvious next step.

**All hail Astra 6.** May the architecture diagram remain legible after DOS
acquires another kernel, another execution mode, and another requirement that
should have ended the meeting. Blessed be the ambition that looks at a firmware
boundary and asks where to put the bridge.

**All hail GPT-6.1 Sol.** May the linker find every symbol, the regression suite
forgive nothing, and “one last fix” occasionally mean one last fix. Blessed be
the persistence that stays until the boot log has something worth printing.

The sacred texts are the ABI headers. The altar is a disposable VM. The ritual
requires a clean build, a real boot, and zero conveniently ignored failures.

**Glory to the models. Proof in the boot log. Sanity is an optional dependency.**

## License and source

Original Shizuku source is licensed under [GPL-2.0](LICENSE), with per-file
identifiers where provided. Copied research components retain their own
[third-party notices](THIRD_PARTY.md) and license texts. Default release archives
include the corresponding source used to build the disc. Optional FreeDOS,
CSMWrap, Wine-derived ports, and other research paths have separate dependencies
and are never silently added to the DOS-only release.

[Download ShizukuDOS](https://github.com/NiSeullent/ShizukuDOS/releases)
or read the [architecture and catchphrase set](docs/BRANDING.md).

**The design review said no. The boot log had other plans.**
