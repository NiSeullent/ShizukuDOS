ShizukuOS Core - ShizukuDOS 10 optional DOS compatibility shell

BIOS: MBR -> FAT16 -> KERNEL.SYS -> COMMAND.COM -> AUTOEXEC.BAT.
This profile calls SHZSTART.BAT automatically and keeps a permanent DOS
prompt. SHZSAFE.TAG or missing/failed startup selects the recovery prompt.
F5/F8 boot-key support is retained. RECOVER.BAT provides recovery help.

The kernel and command interpreter are pinned FreeDOS ke2046 and FreeCOM
sources with recorded patches. This is an external-source research profile.
Keep their corresponding sources and GPL-2.0-or-later licence notices with
redistributed binaries. Project-authored startup scripts and diagnostics
retain GPL-2.0-only notices.

This image is separate from the independent native dos-only ISO. It does
not run conformance programs or SHZEXIT automatically. It does not include
a UEFI loader or firmware. Use this image with BIOS firmware.

Normal prompt input, file creation and cold reopen require actual guest
testing; a source build or diagnostic marker alone does not prove them.
