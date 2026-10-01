# Independent DOS-only ISO

ShizukuDOS has its own source repository and ISO release track. Its source was
copied from the ShizukuDOS components in Windows 98 Shizuku Modern Edition;
building this ISO reads only this repository. The Windows 98 ShkSE ISO remains
a separate product and build track.

The optical image supplies two firmware boot entries:

| Firmware | Boot path | Applications |
| --- | --- | --- |
| BIOS | Original ShizukuDOS real-mode FAT12 shell | COM16; `EXEC DEMO.COM` |
| x86-64 UEFI | Original loader → standalone Kernel64 | Native SD64; HELLO64 and MEM64 |

The BIOS entry does not boot the 64-bit kernel. The UEFI entry requires no VMX,
Supervisor, CSMWrap, GRUB, Windows media or Windows runtime. It embeds the native
kernel and initial RAM archive in the EFI application, exits boot services using
the firmware's final memory map, preserves reserved-memory holes, and hands over
the GOP framebuffer. The unsigned experimental EFI application requires firmware
that accepts it; Secure Boot signing is outside this image's current build.

The UEFI entry is a focused application demonstration: it runs two SD64 programs,
reports guest-generated results on COM1, and halts after completion. It is not yet
a full interactive DOS64 command shell. SD64 is this project's native 64-bit DOS
application ABI, rather than a claim that classic 16-bit DOS binaries execute in
64-bit mode. MEM64 allocates and verifies a user virtual address above 4 GiB;
the validation guest uses 256 MiB of physical RAM.

## Build

Install Python 3, NASM, GCC/binutils with freestanding x86 support,
`x86_64-w64-mingw32-gcc`, xorriso, and mtools. Then run from the repository root:

```sh
python3 tools/build_iso.py
```

This compiles the kernels, builds the SD64 samples and archive, assembles the
BIOS shell, compiles the original UEFI loader, and writes
`build/shizukudos-10-dos-only.iso` and its SHA-256 sidecar. It downloads no source,
starts no services and changes no host boot configuration.

To assemble an image from already-built kernel and sample artifacts:

```sh
python3 tools/build_iso.py --skip-build
```

`--kernel`, `--initrd`, `--out` and `--cmdline` select explicit inputs/output.
The default UEFI command line is `shz.dos64=1`, which selects the native-only
application runner. The same repository is the only source of build inputs.
Publishable images require matching kernel/sample build receipts and source
hashes. Source edits after compilation require a rebuild. The explicit
`--allow-unverified-inputs` option is for exploratory tests and produces an image
marked `inputs_verified=false`; the release packager rejects it.

## Test the delivered ISO

```sh
python3 tools/test_iso.py --qemu /usr/libexec/qemu-kvm
```

`--ovmf-code` and `--ovmf-vars` accept local OVMF firmware paths; on Debian/Ubuntu
these are normally `/usr/share/OVMF/OVMF_CODE.fd` and `OVMF_VARS.fd`. A fresh copy
of the variables image is used for every test. `--layout-only` verifies the
El Torito entries and content hashes without claiming a successful boot.

The full test boots the actual ISO once using BIOS and once using UEFI, with TCG,
no NIC, no host disks and no passthrough. It checks the BIOS prompt, FAT12 file
listing/reading and COM16 execution, then requires firmware handoff, native
Kernel64 entry, HELLO64 64-bit arithmetic, MEM64 memory above 4 GiB, both apps'
successful exit codes, and the guest's zero-failure summary. Serial logs, commands,
ISO hash and check results are saved in `build/iso-tests/`.

QEMU evidence verifies the shipped media paths; it does not certify physical
hardware or every UEFI firmware. The memory layout currently requires fixed
low-memory boot pages, the kernel window at 1 MiB, and the archive at 32 MiB.
Firmware that owns those addresses is rejected instead of overwritten.

## Distribution contents

The ISO includes `SAMPLES/` with the native applications/archive and `SOURCE/`
with GPLv2, the third-party license manifest and complete corresponding source
including build scripts. `MANIFEST.json` identifies both boot profiles and
records hashes of the image contents. External GRUB/CSMWrap code or binaries are
not included. The release workflow uploads this ISO, checksums, build manifest
and actual ISO boot evidence separately from the Windows 98 release track.
