# ShizukuDOS 10.0.1 release scope

10.0.1 repairs the EFI entry and boot-media paths of the independent native
desktop track. 10.0.0 was validated only through BIOS/UEFI virtual CD-ROM;
its raw USB layout and IA32 EFI entry were missing.
The operating-system architecture remains experimental; the release contract
is the functionality checked on its exact ISO bytes, with corresponding source
and evidence attached to the GitHub release.

The IA32/x64 UEFI paths exit firmware boot services and open ShizukuGUI on
the firmware's 32-bit GOP framebuffer. F8 runs three real DOS MZ16 programs,
three PE32/i386 programs and fourteen import-free PE32+/AMD64 programs.
Mode checks read actual CR0, EFER and segment registers. Long-mode EXEs run in
ring 3 with private address spaces; trusted 16/32-bit samples use a privileged,
synchronous mode bridge with masked interrupts. The bridge is not a sandbox
for arbitrary legacy software.

The syscall return selectors include the explicit ring-3 bits required by AMD
SYSRET. The first resolved user page fault verifies the saved CS/SS before
IRETQ; this catches the AMD return-path error that Intel/QEMU previously masked.

The BIOS path remains the original FAT12 shell and its DOS `.COM` examples.
DOS compatibility is the documented INT 21h subset and supplied MZ16 programs,
not every DOS application, extender or driver. Kernel32, the VMX Supervisor,
FreeDOS and CSMWrap remain separate research profiles. Historical reports in
`docs/shizukudos10/` do not certify this release.

ShizukuGUI supports keyboard-driven windows, a live scheduler/thread-tree view,
native utility launching, original RGB24 animation and timed PC-speaker note
scores. Its own HTML parser provides links, scrolling, history and bounded
plain HTTP to numeric IPv4 URLs. TLS, DNS, JavaScript, CSS, compressed video and
general PCM/audio codecs are outside this release. The built-in media and web
fixtures are generated entirely from included source. See
[media/browser contracts](../samples/media/README.md).

The kurazy ABI provides memory, files, timing, console output and real owned
thread trees. Cancellation, parent-only join and malformed user requests are
exercised by native EXEs. The
[versioned specification](../sdk/kurazy/SPEC.md), C header, static library and all
twenty EXE sources are distributed with the image.

Release acceptance uses the exact ISO for BIOS, IA32/x64 UEFI optical, SATA,
raw USB, disk and absent-UART boots under isolated QEMU TCG,
no NIC or host disk, actual framebuffer screenshots and keyboard input. A
formal release additionally requires actual VirtualBox 7 EFI32 and EFI64 DVD
boots with the same ISO hash, desktop interaction and all native mode checks.
A loopback HTTP transaction exercises both ends of the native TCP path. Stock
QEMU also records and checks non-silent PC-speaker audio; distribution builds
that omit the speaker explicitly record that limitation. The test is not
physical-machine certification. GOP, reclaimable conventional-memory windows
and PS/2 keyboard input (or firmware emulation) are required; USB HID, graphics
acceleration and Secure Boot signing are not implemented here.
