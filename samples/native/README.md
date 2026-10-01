# Native 64-bit DOS EXE examples

These are real PE32+/AMD64 programs, compiled as freestanding C and linked to
ShizukuDOS's own static **kurazy** library. They run in ring 3 long mode with
private page tables. They do not import a Windows DLL and do not use Windows
application entry or runtime APIs.

| Program | Implemented work |
|---|---|
| HELLO64 | checks actual long-mode flags and prints a greeting |
| MEM64 | allocates, writes/verifies and frees 64 KiB; rejects double free |
| TIME64 | measures a real scheduler sleep with the monotonic tick clock |
| DIR64 | enumerates the packaged application directory |
| TYPE64 | reads the packaged welcome document and verifies EOF |
| HASH64 | FNV-1a64 streaming file checksum with a known-vector check |
| INFO64 | ABI, memory pages, thread ID and GOP information |
| THREAD64 | real three-generation thread tree, owner checks, subtree cancellation and joins |
| TREE64 | concurrent sibling tasks and owned tree inspection |
| CHECK64 | malformed API request rejection checks |
| CPU64 | executes CPUID and reports the native CPU vendor |
| VIDEO64 | opens the original packaged truecolor KV64 video player |
| MUSIC64 | opens the original KM64 score player and visualization |
| BROWSE64 | opens the original local HTML browser |

The three GUI programs require a UEFI GOP framebuffer. BIOS text boot runs the
11 console/native examples and the two earlier SD64 applications; UEFI GOP boot
runs all 14 EXEs and both SD64 applications. These tests are distinct from the
six real/protected-mode examples and from the interactive desktop lifetime.

Build: `python3 -B tools/build_apps.py`. Artifacts live under `build/native/`.
See `sdk/kurazy/SPEC.md` for the versioned ABI and exact compatibility limits.
