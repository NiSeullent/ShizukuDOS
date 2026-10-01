# DOS-only preview status

This repository has an independent source and ISO release track. The copied
research reports under `docs/shizukudos10/` are historical upstream evidence;
they are not acceptance results for a new build from this repository.

The preview contains the original BIOS FAT12 shell, an original native UEFI
loader, standalone Kernel64, and the HELLO64/MEM64 native samples. It excludes
Windows installation files and Windows wrapper payloads. Kernel32, the VMX
Supervisor, FreeDOS, and CSMWrap remain separately selectable research code.

Release acceptance requires parsing the actual ISO boot catalog, extracting and
comparing payload bytes, booting the BIOS `.COM` path, and booting the UEFI
native sample path with both samples exiting successfully. Saved evidence and
release artifact hashes accompany the GitHub release.

This release does not claim physical-machine certification, complete DOS API
compatibility, an interactive DOS64 shell, or verified Windows 98 execution.
