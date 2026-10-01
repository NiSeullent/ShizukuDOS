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
