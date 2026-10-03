# Reviewed DOS source sync for ShizukuOSCore

The user requested periodic DOS development commits in this independent repository. ShizukuOSCore may use its own DOS shell; Win98-Modern retains genuine Windows98 as its product frontend. Each repository keeps its own history and acceptance evidence.

`tools/sync_dos_sources.py` compares a named, immutable public Win98-Modern commit against this repository's last shared source hashes. Its exact initial allowlist is `shizukudos/boot.asm`, `shizukudos/stage2.asm` and `shizukudos/cpu_detect.inc`. `docs/DOS_SOURCE_SYNC.json` records those original copied hashes. `SOURCE_ORIGIN.json` continues to identify the initial import and is never rewritten by sync.

Every accepted source batch must also append its source revision, copied or adapted file hashes, and actual validation to `docs/SOURCE_UPDATES.json`, as required by `AGENTS.md`. The shared-source ledger does not replace that acceptance history. Existing reviewed optional DOS16 shell batches remain recorded there; this tool's three-path selection does not import or overwrite those files.

The native Core desktop, startup/configuration, Kernel64, ABI, firmware bridge, build tools, drivers, accounts, storage and network development are outside this initial selection. Expanding the selection requires a separate source-owner review, a narrow policy/code change and focused compatibility tests. Directory mirrors, Windows startup/recovery/installer imports, private media, keys, guest images and generated artifacts are outside the workflow. The offline `dos-only` default is unchanged.

## One reviewed source epoch

1. The current DOS producer owner publishes a frozen public commit, exact shared paths and relevant test results through the existing session mailbox. Inspect the actual diff against both repositories' source histories. A Win98 test or branding change does not automatically qualify for the independent Core.
2. Run the read-only comparison on an isolated Core branch. Both-sides-changed conflicts refuse application; target-only changes remain intact. Source-only changes remain proposals until the owner review is supplied.
3. Record the reviewed source handoff as a small JSON object containing exactly `owner`, `approved`, `source_commit` and `paths`. This is the operator's reviewed handoff record, not an authentication token. Its commit must be the full 40-character public source commit; its path list must cover the exact selected files. Keep identifiers public and do not place secrets or unrelated metadata in this record.
4. From a clean, isolated Core branch, apply only that reviewed selection. The tool checks the current target HEAD, policy, initial-origin hash and selected file bytes again before writing. A failed write rolls back the selected files and policy. Review the resulting diff and deterministic source/target/baseline SHA-256 ledger; run the tests appropriate to those actual source changes.
5. Commit and share the tested source epoch with both owners. The existing Core publisher can integrate it; an authorized maintainer may publish the isolated source branch for review. Updating shared DOS sources does not establish a new ISO or guest runtime result.

Repeat this sequence whenever a reviewed shared DOS change becomes ready. This change installs no timer, service, webhook or remote publishing automation, and the sync tool never fetches, builds, commits, pushes or starts a guest.

## Commands

The source checkout is an explicit argument and must have the reviewed Win98-Modern origin. Use an existing frozen public commit rather than `HEAD` or a branch name.

```sh
python3 -B tools/sync_dos_sources.py --source /path/to/Win98-Modern --commit FULL_PUBLIC_COMMIT --path shizukudos/cpu_detect.inc
python3 -B tools/sync_dos_sources.py --source /path/to/Win98-Modern --commit FULL_PUBLIC_COMMIT --path shizukudos/boot.asm --handoff /path/to/reviewed-handoff.json --apply
python3 -B -m unittest discover -s tools/tests -p test_sync_dos_sources.py -v
```

Without `--apply`, the tool prints a deterministic JSON plan and writes no files. An apply requires the reviewed handoff and a clean target; it writes only selected source files and the policy ledger. A source deletion, symlink, executable blob, NUL-bearing binary, oversized file, wrong upstream, unselected path, source-owner mismatch, conflict or target drift is refused. The tool reads committed Git blobs, so dirty or evolving source checkout files cannot silently become imports. All Git reads force `--no-replace-objects` and `GIT_NO_REPLACE_OBJECTS=1`, including alternative replacement namespaces, so an approved commit or blob cannot silently resolve to replacement content. The helper also clears inherited Git configuration/object-location overrides, reads the source origin only from local non-included configuration, and disables fsmonitor hooks; local status checks cannot launch such a hook. It does not reset or clean either checkout.

Every Git read also forces `--no-lazy-fetch` and `GIT_NO_LAZY_FETCH=1`. A missing object in a partial/promisor checkout is refused instead of silently downloading it. Use a Git version that supports `--no-lazy-fetch`; an unsupported option refuses the local read. Obtain required public objects separately before running the comparison. The tool does not acquire them or change either checkout's configuration.

The initial workflow commit imports no upstream source. Current boot/stage2 differences need the producer owner's specific review. Focused tests use small local Git repositories and public text fixtures, including a partial clone with a local fake promisor transport. They establish source-selection, conflict, drift, rollback and provenance behavior, with no Windows, Core desktop, physical hardware, VM or ISO execution claim.
