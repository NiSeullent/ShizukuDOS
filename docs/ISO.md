# Independent DOS-only ISO

ShizukuDOS has its own source history and DOS-only release track. The Windows
98 ShkSE source and ISO remain separate. Default builds use only this repository.

| Boot path | Delivered behavior |
| --- | --- |
| BIOS optical | Original FAT12 Real Mode shell and COM examples |
| IA32 UEFI optical | BOOTIA32.EFI enters the native x86-64 kernel on a capable CPU |
| x64 UEFI optical | BOOTX64.EFI enters the same native kernel |
| UEFI USB or disk | GPT exposes the embedded FAT32 EFI System Partition and both removable EFI entries |

10.0.0 was tested as optical media only and omitted an IA32 EFI entry and disk
partition metadata. 10.0.1 repairs those paths. The loader stages the payloads
and reserves its own executable gateway and stack, exits boot services, checks
the final firmware memory map, then reclaims permitted boot and kernel windows.
It does not overwrite live firmware memory or execute a LoaderData allocation.
Runtime, reserved, ACPI NVS and MMIO ranges remain excluded.
The kernel also fixes the AMD SYSRET stack selector: the ring-3 bits are explicit
in STAR rather than relying on Intel's behavior. This prevents MEM64's first
demand-page fault from failing when IRETQ restores the user stack segment.

The desktop opens before compatibility diagnostics. Press F8 to run all mode,
native executable, API, graphics and media checks. Failures remain visible;
F10 shuts down. ShizukuGUI uses the firmware GOP framebuffer directly.

## VirtualBox 7.x

Use an x86 virtual machine with Long Mode enabled, 256 MiB RAM, EFI firmware,
a SATA/AHCI-attached DVD containing the ISO and Secure Boot disabled. Both
EFI32 and EFI64 entry files are included; the native kernel still requires an
x86-64 CPU. The memory plan requires at least 64 MiB of usable RAM after
firmware reservations. 256 MiB avoids the ambiguity of a minimal DOS VM preset.

The included isolated harness creates its own temporary VM configuration and
runs without a NIC, hard disk, shared folders or Guest Additions:

```sh
python3 -B tools/test_virtualbox.py --iso build/shizukudos-10-dos-only.iso
```

It requires an already installed VirtualBox 7 with usable hardware
virtualization. It does not install host modules or alter existing VMs. Host
unavailability is recorded as UNAVAILABLE, never PASS. The dedicated GitHub
workflow installs tools only on its disposable runner and tests EFI32 and
EFI64 from the exact same ISO.

## Build and verify

Install Python 3, NASM, GCC/binutils, x64 and i686 MinGW, xorriso and mtools.
QEMU plus matching x64 and IA32 OVMF firmware pairs are needed for acceptance.
Ubuntu packages provide `ovmf` and `ovmf-ia32`.

```sh
python3 -B tools/build_iso.py
python3 -B tools/test_iso.py --require-audio
python3 -B samples/dos64/test_runtime.py
python3 -B tools/test_virtualbox.py
python3 -B tools/package_release.py --version 10.0.1
```

The builder creates the standalone kernel, fourteen PE32+ EXEs, the static
kurazy library, six MZ16/PE32 examples, original media and both EFI loaders.
Outputs remain under ignored build directories. It fetches no source, changes
no host boot settings and needs no Windows installation media.

Release acceptance boots the exact ISO as BIOS optical, x64 optical, raw USB,
AHCI disk, q35 SATA DVD, IA32 optical and an x64 guest without a serial port.
It checks GPT and FAT32, both PE EFI architectures, source/payload hashes,
actual CPU registers, all supplied programs, malformed inputs, actual
thread-tree operations, GOP readback, HTTP/TCP, keyboard navigation and
recorded non-silent PC-speaker audio. The desktop must become interactive
before F8 diagnostics. Every guest has a fresh variables image and no NIC or
host disk. Evidence includes logs, screenshots, audio and result JSON.

`--ovmf-code`, `--ovmf-vars`, `--ovmf-ia32-code` and `--ovmf-ia32-vars` override
firmware paths. `--profile` selects a focused regression; release packaging
requires every profile. `--layout-only` proves only the layout. A clean source
commit and exact tested bytes are required for packaging. Formal packaging also
requires actual VirtualBox 7 EFI32 and EFI64 results for the same ISO hash.
`--virtualbox-evidence` selects downloaded results from the isolated workflow.
`--candidate` creates an explicitly unpublished candidate before that check;
its manifest records VirtualBox as NOT_RUN.

`--skip-build` accepts only matching source/binary receipts. Explicit kernel,
initrd, output and command-line paths are supported. Exploratory
`--allow-unverified-inputs` images cannot be packaged as official releases.
The full multi-kernel research build remains separately available.

## Desktop and scope

F1 browser, F2 video, F3 music, F4 thread tree, F5 utilities, F6 URL input,
F8 diagnostics and F10 shutdown. Space pauses/resumes players; Tab/Enter
selects/opens; Backspace returns through browser history; arrows scroll.
Keyboard input requires PS/2 or firmware emulation. All graphics use GOP.

The original browser supports bounded HTML and HTTP/1.0 to numeric IPv4 hosts.
External requests need a supported network interface; isolated acceptance uses
actual native TCP loopback. Video uses KV64 RGB frame animation; music uses
KM64 PC-speaker scores. DOS compatibility covers the documented INT21 subset
and trusted examples. See [media contracts](../samples/media/README.md),
[kurazy](../sdk/kurazy/SPEC.md) and [release scope](STATUS.md).

SAMPLES contains twenty EXEs, two SD64 programs and the exact executed archive.
SOURCE contains complete corresponding source and license notices. Release
assets include the ISO, source archive, apps/static SDK, evidence, manifest and
SHA256SUMS. Physical-machine certification and Secure Boot signing are not
claimed by these virtual-machine checks.
