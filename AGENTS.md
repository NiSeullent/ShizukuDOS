# ShizukuDOS independent DOS / Core development

**ShizukuOS is the root operating-system platform. ShizukuOS Core is its common
execution base, and ShizukuDOS is a subordinate DOS-compatible component.**
Historical implementation origins do not define system boundaries. The full
system definition belongs to the parent [ShizukuOS architecture contract](https://github.com/NiSeullent/ShizukuOS/blob/main/docs/SHIZUKUOS_ARCHITECTURE_CONTRACT.md).
Do not duplicate that goal specification here.

This independent repository maintains the DOS shell and reviewed portable
Core services. ShizukuOS's native desktop shell, Slade/Flute/Jade theme system,
full runtime/application ecosystem, Linux and ShizukuVM product integration,
installer/product ISO and official website belong to the parent ShizukuOS
repository. Existing standalone DOS examples and experimental DOS-only ISO
retain their specific evidence and scope; they do not establish a complete
ShizukuOS installation. Existing directory names and licensed research sources
remain until actual incremental conversion; no wholesale mirror or parallel
implementation is implied by the platform definition.

`SOURCE_ORIGIN.json` records the original independent import and is immutable.
Preserve historical source and validation records without relabeling their
scope. Before porting, classify existing code and inspect actual callers,
providers, build wiring and standalone adaptations. ReactOS and Wine are
permitted references subject to licensing, provenance and this repository's
DOS/Core boundary.

Default builds use `python3 -B shizukudos/tools/shz.py build --profile dos-only`.
They remain offline and do not require the parent checkout, Windows media,
upstream downloads, host boot changes or client-global configuration. Optional
FreeDOS/FreeCOM/CSM research profiles are explicit and retain their independent
source pins/licenses. Preserve the optional BIOS-only `--user-shell` behavior;
parent Windows/profile build choices must not overwrite it.

Keep generated files in ignored build directories. Never commit private media,
credentials, VM state, app archives, local machine configuration or runtime
coordination notes. Include corresponding source and complete license texts
for actual distributed components.

## Periodic source updates and acceptance

Review and port coherent DOS or portable Core changes from ShizukuOS in regular
exact-path batches. Preserve standalone changes instead of replacing whole
directories. Record immutable source and target revisions, file hashes,
adaptations and actual checks in append-only `docs/SOURCE_UPDATES.json`.
The historical three-BIOS-file sync tool has its own narrower policy; it does
not grant admission to additional Core/ABI/profile paths.

Run meaningful existing checks for the actual changed production code. Shared
IPC work uses `python3 -B shizukudos/abi/test_abi.py` and affected production-unit
compilation. Full DOS ISO acceptance uses
`python3 -B shizukudos/tools/shz.py test --suite iso` when a release changes.
Firmware layout, actual BIOS execution, UEFI sample execution, host models and
actual ShizukuOS feature acceptance are separate results. Report only checks
that ran; preserve failures. Run bounded owned guests without a NIC and protect
other sessions' guests and source edits. Publish reviewed, tested batches
without force-overwriting foreign remote changes. The parent OS publisher owns
its main branch, product release and nginx site at https://m98.nyase.kr.
