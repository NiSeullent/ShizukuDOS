#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline import-free AMD64 PE32+ native EXEs, static kurazy SDK, initrd and receipt."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "sdk/kurazy"
APPS = ("HELLO64", "MEM64", "TIME64", "DIR64", "TYPE64", "HASH64", "INFO64", "THREAD64", "TREE64", "CHECK64", "CPU64", "VIDEO64", "MUSIC64", "BROWSE64")
FLAGS = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-ident", "-mno-red-zone", "-mgeneral-regs-only", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-I", str(SDK / "include")]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

def run(command):
    subprocess.run([str(x) for x in command], cwd=ROOT, check=True)

def inspect_pe(path):
    """Check emitted image identity and reject actual imported DLL descriptors."""
    b = path.read_bytes()
    if b[:2] != b"MZ":
        raise ValueError("not a DOS/PE executable: " + str(path))
    nt = struct.unpack_from("<I", b, 0x3c)[0]
    if b[nt:nt+4] != b"PE\0\0" or struct.unpack_from("<H", b, nt+4)[0] != 0x8664:
        raise ValueError("not AMD64 PE: " + str(path))
    opt = nt+24
    if struct.unpack_from("<H", b, opt)[0] != 0x20b:
        raise ValueError("not PE32+: " + str(path))
    sections = struct.unpack_from("<H", b, nt+6)[0]
    table = opt + struct.unpack_from("<H", b, nt+20)[0]
    def file_offset(rva):
        for i in range(sections):
            s = table + i*40
            va, raw_size, raw = struct.unpack_from("<III", b, s+12)
            if va <= rva < va+raw_size:
                return raw+rva-va
        raise ValueError("invalid directory RVA")
    imports, import_size = struct.unpack_from("<II", b, opt+112+8)
    if imports:
        off = file_offset(imports)
        # GNU ld emits an empty .idata sentinel even for -nostdlib.
        if any(b[off:off+20]):
            raise ValueError("native EXE has a DLL import")
    for idx in (9, 10, 13, 14):
        if struct.unpack_from("<II", b, opt+112+8*idx) != (0,0):
            raise ValueError("unsupported TLS/load-config/delay/CLR runtime directory")
    return {"format": "PE32+ AMD64 native kurazy", "machine": "0x8664", "optional_header": "0x20b", "dll_dependencies": [], "entry_rva": hex(struct.unpack_from("<I", b, opt+16)[0]), "image_base": hex(struct.unpack_from("<Q", b, opt+24)[0]), "bytes": len(b)}

def source_paths():
    paths = [Path(__file__).resolve(), ROOT / "samples/dos64/build.py", ROOT / "tools/build_mode_apps.py"]
    for directory in (ROOT / "samples/native", SDK, ROOT / "samples/media", ROOT / "samples/modes"):
        if directory.is_dir():
            paths.extend(p for p in directory.rglob("*") if p.is_file() and "__pycache__" not in p.parts)
    paths.extend(ROOT / "samples/dos64" / p for p in ("hello64.asm", "mem64.asm", "abi.inc"))
    return sorted(set(paths))

def build(out):
    for tool in ("x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-ar", "nasm"):
        if not shutil.which(tool):
            raise RuntimeError("required installed offline tool missing: " + tool)
    out.mkdir(parents=True, exist_ok=True)
    mode_builder = ROOT / "tools/build_mode_apps.py"
    if mode_builder.is_file():
        run(["python3", "-B", mode_builder])
    paths = source_paths()
    before = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    cc = "x86_64-w64-mingw32-gcc"
    objects = []
    commands = []
    for source in (SDK / "src/kurazy64.c", SDK / "src/syscall64.S"):
        obj = out / (source.stem + ".o")
        cmd = [cc, *FLAGS, "-c", source, "-o", obj]
        run(cmd); commands.append([str(x) for x in cmd]); objects.append(obj)
    library = out / "libkurazy64.a"
    run(["x86_64-w64-mingw32-ar", "rcsD", library, *objects])
    apps = {}
    files = []
    for name in APPS:
        exe = out / (name + ".EXE")
        cmd = [cc, *FLAGS, "-nostdlib", "-Wl,--no-insert-timestamp,--image-base,0x400000,--entry,kurazy_start,--subsystem,native,--disable-dynamicbase,--nxcompat,--file-alignment,512,--section-alignment,4096", SDK / "src/start64.c", ROOT / "samples/native" / (name.lower()+".c"), library, "-o", exe]
        run(cmd); commands.append([str(x) for x in cmd])
        record = inspect_pe(exe)
        record.update(file=str(exe), sha256=sha(exe), archive_path="\\SHZ\\DOS64\\"+exe.name)
        apps[exe.name] = record
        files.append((record["archive_path"], exe.read_bytes()))
    legacy = module(ROOT / "samples/dos64/build.py", "legacy_samples")
    legacy.build(ROOT / "build/dos64")
    for name in ("HELLO64.SD64", "MEM64.SD64"):
        path = ROOT / "build/dos64" / name
        files.append(("\\SHZ\\DOS64\\"+name, path.read_bytes()))
    for path in sorted((ROOT / "build/modes").glob("*.EXE")):
        files.append(("\\SHZ\\MODES\\"+path.name, path.read_bytes()))
    media_builder = ROOT / "samples/media/build.py"
    if media_builder.is_file():
        media_out = out / "media"
        run(["python3", "-B", media_builder, "--output", media_out])
        for path in sorted(media_out.rglob("*")):
            if not path.is_file() or path.suffix == ".json":
                continue
            rel = path.relative_to(media_out)
            location = "\\"+str(rel).replace("/", "\\") if rel.parts[0] == "WWW" else "\\MEDIA\\"+path.name
            files.append((location, path.read_bytes()))
    files.append(("\\DOCS\\WELCOME.TXT", b"ShizukuDOS 10.0.1: native DOS apps in real, protected and long modes.\r\nkurazy is the import-free native SDK. ShizukuGUI renders GOP truecolor.\r\nGlory to the models. Proof in the boot log. Sanity is optional.\r\n"))
    for path in sorted(SDK.glob("*.md")):
        files.append(("\\DOCS\\"+path.name.upper(), path.read_bytes()))
    files.append(("\\SDK\\KURAZY.H", (SDK / "include/kurazy.h").read_bytes()))
    files.append(("\\SDK\\LIBKURAZY64.A", library.read_bytes()))
    initrd = out / "DOS64.IMG"
    initrd.write_bytes(legacy.pack_archive(files))
    after = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    if before != after:
        raise RuntimeError("native SDK/sample sources changed while building; rebuild stable sources")
    receipt = {"abi": "kurazy 1.0 + SD64 compatibility", "apps": apps, "sdk": {"file": str(library), "sha256": sha(library)}, "archive": {"file": str(initrd), "sha256": sha(initrd), "bytes": initrd.stat().st_size, "files": len(files)}, "source_files_sha256": before, "commands": commands}
    (out / "build-result.json").write_text(json.dumps(receipt, indent=2)+"\n")
    return receipt

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/native")
    args = parser.parse_args()
    result = build(args.out.resolve())
    print(json.dumps({"native_exes": len(result["apps"]), "archive": result["archive"], "sdk": result["sdk"]}, indent=2))
if __name__ == "__main__":
    main()
