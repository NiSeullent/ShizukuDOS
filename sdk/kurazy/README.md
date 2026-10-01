# kurazy SDK

An original freestanding API for ShizukuDOS's three native CPU-mode tracks.
[The 1.0 API specification](SPEC.md) states the actual supported calls and limits.

Build all 14 native x86-64 `.EXE` applications and the static library:

```sh
python3 -B tools/build_apps.py
```

Installed offline dependencies: Python, NASM, and the MinGW-w64 GCC/binutils
cross compiler. Native executables are genuine AMD64 PE32+ images with no
Windows CRT or imported DLLs. The build emits `build/native/libkurazy64.a`,
`*.EXE`, a manifest and `DOS64.IMG`, which also packages the earlier SD64 apps,
available real/protected-mode fixtures, original GUI media and documentation.

To create a new long-mode app, provide `int app_main(void)` and link its source
with `src/start64.c` and `libkurazy64.a` using the flags from
`tools/build_apps.py`. File names end in `64.EXE`. Thread callbacks explicitly
call `kurazy_exit(code)`. Never treat these native images as Windows programs.

A minimal sample:

```c
#include "kurazy.h"
int app_main(void) {
    kurazy_info info;
    if (kurazy_query(&info) || info.mode != KURAZY_MODE_LONG64) return 1;
    kurazy_puts("Hello from a real native 64-bit DOS application.\n");
    return 0;
}
```

Read and negative-test the emitted PE files with:

```sh
python3 -B samples/native/test_native.py
```

The host checks verify image format and shared parser rejection. Runtime mode,
thread ownership and GUI claims require the isolated BIOS/UEFI guest evidence
produced by the ISO test suite.
