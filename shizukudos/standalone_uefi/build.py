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


def build(kernel, initrd, output, cmdline="shz.dos64=1", architecture="x64"):
    if architecture not in ("x64", "ia32"):
        raise ValueError("architecture must be x64 or ia32")
    target = "x86_64" if architecture == "x64" else "i686"
    compiler = shutil.which(target + "-w64-mingw32-gcc")
    if not compiler:
        raise RuntimeError("required tool missing: " + target + "-w64-mingw32-gcc")
    if not cmdline.isascii() or any(ord(c) < 32 or ord(c) >= 127 for c in cmdline) or len(cmdline) >= 256:
        raise ValueError("command line must contain fewer than 256 printable ASCII bytes")
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    obj = output.parent / ("obj-" + architecture)
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
        symbol = ("_" if architecture == "ia32" else "") + "dos_" + name
        text += f'.balign 16\n.globl {symbol}_start\n.globl {symbol}_end\n{symbol}_start:\n.incbin "{path}"\n{symbol}_end:\n'
    asm.write_text(text)
    command = [compiler, "-std=gnu11", "-Os", "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin",
               "-fno-stack-protector", "-mno-stack-arg-probe", "-fno-ident",
               "-fno-asynchronous-unwind-tables", "-fno-tree-loop-distribute-patterns", "-ffunction-sections",
               "-fdata-sections", "-nostdlib", "-Wl,--subsystem,10", "-Wl,--entry," + ("_efi_main" if architecture == "ia32" else "efi_main"), "-Wl,--image-base,0x10000000",
               "-Wl,--enable-reloc-section", "-Wl,--no-insert-timestamp", "-Wl,--strip-all", "-Wl,--gc-sections",
               "-I", str(obj), str(REPO / "shizukudos/standalone_uefi/loader.c"),
               str(REPO / "shizukudos/uefi/boot.c"), str(asm), "-o", str(output)]
    command[1:1] = ["-mno-red-zone"] if architecture == "x64" else ["-malign-double", "-mno-sse", "-mno-mmx", "-msoft-float"]
    subprocess.run(command, check=True)
    data = output.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    assert data[:2] == b"MZ" and data[pe:pe + 4] == b"PE\0\0"
    assert struct.unpack_from("<H", data, pe + 4)[0] == (0x8664 if architecture == "x64" else 0x14c)
    assert struct.unpack_from("<H", data, pe + 24)[0] == (0x20b if architecture == "x64" else 0x10b)
    assert struct.unpack_from("<H", data, pe + 24 + 68)[0] == 10
    (output.parent / "build-result.json").write_text(json.dumps({"command": command, "bytes": len(data),
                                                               "architecture": architecture, "cmdline": cmdline}, indent=2) + "\n")
    return output


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--kernel", type=Path, default=REPO / "build/shizukudos/kernel64s/KERNEL64S.BIN")
    p.add_argument("--initrd", type=Path, default=REPO / "build/native/DOS64.IMG")
    p.add_argument("--out", type=Path, default=REPO / "build/standalone-uefi/BOOTX64.EFI")
    p.add_argument("--cmdline", default="shz.dos64=1")
    p.add_argument("--arch", choices=("x64", "ia32"), default="x64")
    args = p.parse_args()
    print(build(args.kernel, args.initrd, args.out, args.cmdline, args.arch))
