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
python3 -B shizukudos/tools/shz.py package --version 10.0.0-preview.1
```

The default build uses installed NASM, GCC/binutils, x64 MinGW, mtools, and
xorriso. It writes generated artifacts under `build/`, fetches no upstream
source, and changes no host boot or client configuration. QEMU and OVMF are
needed for the boot checks. See [ISO instructions](docs/ISO.md).

The ISO boots the original 16-bit FAT12 shell on BIOS and the independent
Kernel64 directly on x64 UEFI. Its UEFI demo profile runs the native DOS64
samples. Those samples use ShizukuDOS's experimental 64-bit syscall ABI;
they are not classic `.COM` programs. The default disc contains no Windows
installation payload, Windows wrapper DLL package, FreeDOS, CSMWrap, or GRUB.

## Execution environments

| Component | Status and boundary |
| --- | --- |
| Original Real Mode shell | Implemented BIOS FAT12 shell with a small `INT 21h` subset and `.COM` execution. General DOS compatibility remains limited. |
| Native DOS64 apps | Experimental x86-64 flat binaries, loaded into user processes by Kernel64. Sources and ABI are in [samples/dos64](samples/dos64/README.md). |
| Protected Mode Kernel32 | Separate implemented i486 kernel source and standalone boot stub; built alongside Kernel64. Its guest execution is outside this release's acceptance checks. |
| Long Mode Kernel64 | Implemented x86-64 kernel and user-process infrastructure. The default UEFI profile runs directly, without the Supervisor or VMX. |
| Virtual Real Mode | Optional Intel VMX Supervisor and pinned FreeDOS research profile. Requires its own build, hardware prerequisites, and validation. |
| CSMWrap | Optional external firmware bridge, with its own source pins and licenses. Excluded from the default ISO. |
| Legacy Windows integration | Source lineage and shared experimental contracts remain available. This DOS release does not install, boot, or replace the Windows 98 DOS core. |

The default release is a bootable developer preview: a BIOS shell and a UEFI
native-sample runner, rather than a complete general-purpose DOS system. The
UEFI profile currently has serial output and no interactive DOS64 command shell.
Hardware coverage, general DOS program compatibility, and simultaneous
multi-kernel execution need separate evidence. See [current release status](docs/STATUS.md).

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

## License and source

Original Shizuku source is licensed under [GPL-2.0](LICENSE), with per-file
identifiers where provided. Copied research components retain their own
[third-party notices](THIRD_PARTY.md) and license texts. Default release archives
include the corresponding source used to build the disc. Optional FreeDOS,
CSMWrap, Wine-derived ports, and other research paths have separate dependencies
and are never silently added to the DOS-only release.

[Download the developer preview](https://github.com/NiSeullent/ShizukuDOS/releases)
or read the [architecture and catchphrase set](docs/BRANDING.md).

**The design review said no. The boot log had other plans.**
