# ShizukuDOS standalone development

This repository is an independent source copy, not a submodule of Win98-Modern.
`SOURCE_ORIGIN.json` identifies the initial source and copied hashes. Preserve
the original project and its separate Windows ISO release track.

Default builds use `python3 -B shizukudos/tools/shz.py build --profile dos-only`.
They must remain offline and must not require a Windows checkout, Windows
media, upstream downloads, host boot changes, or client-global configuration.
Optional external research profiles are explicitly selected and keep source
pins and licensing separate from the default release.

Keep generated files under ignored build directories. Do not put private media,
credentials, VM state, third-party application archives, or host configuration
into source or release packages. Include complete corresponding source and
license texts for the programs actually distributed.

Acceptance: `python3 -B shizukudos/tools/shz.py test --suite iso`. Firmware
layout, actual BIOS execution, and actual UEFI native sample execution are
different checks. Report their evidence and scope accurately. Run only bounded,
isolated guests without a NIC; preserve other sessions' guests and source edits.

## Scope and ongoing DOS updates

Treat this repository as the independent DOS shell and ShizukuOS Core source
track. Keep Windows 98 installation, VMM/NT integration, application ports and
the Windows product ISO in Win98-Modern. Its official distribution site remains
https://m98.nyase.kr. Preserve existing research history without promoting it
into the standalone default release.

When a coherent DOS change lands in Win98-Modern, review and port the relevant
DOS source here too. Commit and push each reviewed batch after its relevant
tests; do not rely on a one-time source copy. Preserve standalone changes rather
than overwriting whole directories. Record the source commit, copied/adapted
file hashes and actual validation in docs/SOURCE_UPDATES.json. Do not rewrite
SOURCE_ORIGIN.json, which describes the original import.

The optional DOS16 user-shell profile uses pinned FreeDOS/FreeCOM sources and
retains their licenses. It does not replace the offline native dos-only default
or establish complete DOS compatibility. Never copy private media, build
outputs or Windows-specific patches as part of a DOS synchronization.
