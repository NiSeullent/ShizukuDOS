#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the original DOS-only UEFI loader with embedded standalone kernel/apps."""
import argparse
import json
import shutil
import struct
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def build(kernel, initrd, output, cmdline="shz.dos64=1"):
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if not compiler:
        raise RuntimeError("required tool missing: x86_64-w64-mingw32-gcc")
    if not cmdline.isascii() or any(ord(c) < 32 or ord(c) >= 127 for c in cmdline) or len(cmdline) >= 256:
        raise ValueError("command line must contain fewer than 256 printable ASCII bytes")
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    obj = output.parent / "obj"
    obj.mkdir(exist_ok=True)
    (obj / "config.h").write_text("#define DOS_CMDLINE " + json.dumps(cmdline) + "\n")
    # .incbin avoids C compiler work proportional to the initial RAM archive size.
    asm = obj / "images.S"
    text = ".section .rdata,\"dr\"\n"
    for name, path in (("kernel", kernel), ("initrd", initrd)):
        path = Path(path).resolve()
        if not path.is_file():
            raise FileNotFoundError(path)
        if '"' in str(path) or "\\" in str(path):
            raise ValueError("payload paths cannot contain assembler escape characters")
        text += f'.balign 16\n.globl dos_{name}_start\n.globl dos_{name}_end\ndos_{name}_start:\n.incbin "{path}"\ndos_{name}_end:\n'
    asm.write_text(text)
    command = [compiler, "-std=gnu11", "-Os", "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin",
               "-fno-stack-protector", "-mno-red-zone", "-mno-stack-arg-probe", "-fno-ident",
               "-fno-asynchronous-unwind-tables", "-fno-tree-loop-distribute-patterns", "-ffunction-sections",
               "-fdata-sections", "-nostdlib", "-Wl,--subsystem,10", "-Wl,--entry,efi_main", "-Wl,--image-base,0x10000000",
               "-Wl,--enable-reloc-section", "-Wl,--no-insert-timestamp", "-Wl,--strip-all", "-Wl,--gc-sections",
               "-I", str(obj), str(REPO / "shizukudos/standalone_uefi/loader.c"),
               str(REPO / "shizukudos/uefi/boot.c"), str(asm), "-o", str(output)]
    subprocess.run(command, check=True)
    data = output.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    assert data[:2] == b"MZ" and data[pe:pe + 4] == b"PE\0\0"
    assert struct.unpack_from("<H", data, pe + 4)[0] == 0x8664
    assert struct.unpack_from("<H", data, pe + 24 + 68)[0] == 10
    (output.parent / "build-result.json").write_text(json.dumps({"command": command, "bytes": len(data),
                                                               "cmdline": cmdline}, indent=2) + "\n")
    return output


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--kernel", type=Path, default=REPO / "build/shizukudos/kernel64s/KERNEL64S.BIN")
    p.add_argument("--initrd", type=Path, default=REPO / "build/dos64/DOS64.IMG")
    p.add_argument("--out", type=Path, default=REPO / "build/standalone-uefi/BOOTX64.EFI")
    p.add_argument("--cmdline", default="shz.dos64=1")
    args = p.parse_args()
    print(build(args.kernel, args.initrd, args.out, args.cmdline))
