# ShizukuDOS 10.0.0 release scope

10.0.0 is the first regular release of the independent native desktop track.
The operating-system architecture remains experimental; the release contract
is the functionality checked on its exact ISO bytes, with corresponding source
and evidence attached to the GitHub release.

The UEFI path exits firmware boot services, executes three real DOS MZ16
programs, three PE32/i386 programs and fourteen import-free PE32+/AMD64 programs,
then leaves ShizukuGUI interactive on the firmware's 32-bit GOP framebuffer.
Mode checks read actual CR0, EFER and segment registers. Long-mode EXEs run in
ring 3 with private address spaces; trusted 16/32-bit samples use a privileged,
synchronous mode bridge with masked interrupts. The bridge is not a sandbox
for arbitrary legacy software.

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

Release acceptance uses actual BIOS and UEFI ISO boots under isolated QEMU TCG,
no NIC or host disk, actual framebuffer screenshots and keyboard input. A
loopback HTTP transaction exercises both ends of the native TCP path. Stock
QEMU also records and checks non-silent PC-speaker audio; distribution builds
that omit the speaker explicitly record that limitation. The test is not
physical-machine certification. GOP, fixed conventional-memory allocations
and PS/2 keyboard input (or firmware emulation) are required; USB HID, graphics
acceleration and Secure Boot signing are not implemented here.
