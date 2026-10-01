# Independent DOS-only ISO

ShizukuDOS has its own repository, source history and release track, copied
from the DOS components of Windows 98 Shizuku Modern Edition. Builds read only
this repository. The Windows 98 ShkSE ISO is a separate product.

| Firmware | Delivered path |
| --- | --- |
| BIOS | Original FAT12 Real Mode shell; `HELP`, `DIR`, `TYPE HELLO.TXT`, `EXEC DEMO.COM` |
| x64 UEFI | Original EFI loader → Kernel64 → actual MZ16/PE32 mode examples → PE32+ utility suite → interactive ShizukuGUI |

The EFI loader embeds the kernel and initial RAM archive, captures GOP, reserves
its memory, and exits boot services using the final firmware memory map. The
kernel owns the CPU tables and page tables from that point. The mode bridge
temporarily leaves Long Mode, disables paging and either retains protected
32-bit execution or clears CR0.PE for native Real Mode. It restores descriptors,
paging, stack and interrupt state before returning to the desktop. No VMX,
CSM, external bootloader or Windows runtime is required by this path.

## Build and verify

Install Python 3, NASM, GCC/binutils with freestanding x86 support, x64 MinGW,
xorriso and mtools. QEMU and OVMF are needed for acceptance. From the root:

```sh
python3 -B tools/build_iso.py
python3 -B tools/test_iso.py
python3 -B samples/dos64/test_runtime.py
python3 -B tools/package_release.py --version 10.0.0
```

The builder compiles the standalone native kernel, fourteen PE32+ EXEs,
`libkurazy64.a`, six MZ16/PE32 examples, the old SD64 samples and original media
fixtures. It then builds the BIOS shell and EFI loader. Outputs remain under
`build/`, including `shizukudos-10-dos-only.iso` and its SHA-256 sidecar.
No source downloads or host boot changes are performed. A clean source commit
is required for release packaging.

`--skip-build` assembles previously built artifacts only when build receipts
match the current source and binaries. `--kernel`, `--initrd`, `--out` and
`--cmdline` accept explicit paths. The default command line is
`shz.dos64=1 shz.gui=1`; it runs startup diagnostics and stays in the desktop
until F10. `shz.modes=1` is the bounded mode-only diagnostic path. The explicit
`--allow-unverified-inputs` option marks exploratory builds unverified; the
release packager rejects them. The full multi-kernel research build remains
available through `python3 shizukudos/kbuild.py`.

The test extracts the actual ISO, validates the boot catalog and exact file
hashes, and checks the corresponding source archive. It boots one isolated
guest at a time with no NIC or host disks. The BIOS check uses the shell and
COM16 program. The UEFI check requires six legacy examples, all fourteen native
EXEs, the old arithmetic/memory checks, negative loader/API checks, actual
thread-tree operations, GOP framebuffer readback and media/browser tests. It
injects keyboard input through a local QEMU control socket, captures the five
panes and a followed HTML page, then shuts down through F10.

`--ovmf-code` and `--ovmf-vars` override local firmware paths. Matching firmware
pairs are detected on RPM and Debian/Ubuntu installations; every test gets a
fresh variables copy. `--layout-only` is explicitly layout-only evidence.
Checks, guest logs, screenshots and optional PC-speaker WAV are saved under
`build/iso-tests/`. Stock QEMU can record actual speaker output; builds without
PC-speaker emulation report that fact in the test result.

## Desktop controls and scope

F1 browser, F2 video, F3 music, F4 thread tree, F5 utilities, F6 browser address,
F10 shutdown. Space pauses/resumes video/music; Tab/Enter selects and opens;
Backspace follows browser history; arrows scroll. Keyboard support currently
requires PS/2/i8042 or firmware emulation. All graphics use GOP only.

The browser supports its documented HTML subset and bounded HTTP/1.0 to
numeric IPv4 addresses. It needs an available supported network interface for
remote requests; the acceptance transaction uses native loopback without a
NIC. The video and music formats are intentionally small and source-contained.
See [media/browser details](../samples/media/README.md) and
[kurazy](../sdk/kurazy/SPEC.md).

## Distribution

`SAMPLES/` contains twenty EXEs, two legacy SD64 programs and the exact initial
RAM archive. `SOURCE/` contains the complete corresponding source, GPLv2 and
third-party notices. `MANIFEST.json` records both profiles and every payload
hash. Release assets include the ISO, source archive, native apps/static SDK,
boot evidence, release manifest and SHA256SUMS.

Firmware must accept the unsigned EFI application and provide a 32-bit GOP
framebuffer. Fixed allocations include boot pages at 0x1000, the mode bridge at
0x10000..0x6ffff, kernel memory at 1 MiB and archive at 32 MiB. Unavailable ranges
are rejected instead of overwritten. This acceptance scope does not certify
physical hardware or general DOS/Windows binary compatibility.
