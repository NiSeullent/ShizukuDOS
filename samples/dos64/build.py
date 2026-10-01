#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build native SD64 applications and a DOS-only SHZARC01 initial RAM archive.

Requires NASM only. No Windows runtime, network download, or target machine.
Outputs build/dos64/HELLO64.SD64, MEM64.SD64, DOS64.IMG and build-result.json.
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).resolve().parent


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pack_archive(files):
    """Kernel64 fs.c format: 16-byte header, 136-byte entries, aligned file data."""
    header_size = 16 + 136 * len(files)
    blob, entries = bytearray(), []
    for path, data in files:
        while (header_size + len(blob)) % 16:
            blob.append(0)
        entries.append((path, header_size + len(blob), len(data)))
        blob.extend(data)
    archive = bytearray(b"SHZARC01" + struct.pack("<II", len(files), 0))
    for path, offset, size in entries:
        raw = path.encode("ascii")
        if len(raw) >= 120:
            raise ValueError("archive path too long")
        archive.extend(raw.ljust(120, b"\0") + struct.pack("<QQ", offset, size))
    return bytes(archive + blob)


def build(out):
    source_files = [SOURCE / "hello64.asm", SOURCE / "mem64.asm", SOURCE / "abi.inc", SOURCE / "build.py"]
    before = {str(p.relative_to(ROOT)): sha256(p) for p in source_files}
    if not shutil.which("nasm"):
        raise SystemExit("required tool missing: nasm")
    out.mkdir(parents=True, exist_ok=True)
    apps, files = {}, []
    for name in ("hello64", "mem64"):
        source, binary = SOURCE / f"{name}.asm", out / f"{name.upper()}.SD64"
        command = ["nasm", "-f", "bin", "-w+all", "-I", str(SOURCE) + "/", "-o", str(binary), str(source)]
        subprocess.run(command, check=True)
        data = binary.read_bytes()
        if not data or len(data) > 65536 or data[:2] == b"MZ":
            raise ValueError(f"invalid native flat application: {binary}")
        archive_path = f"\\SHZ\\DOS64\\{binary.name}"
        files.append((archive_path, data))
        apps[binary.name] = {"format": "SD64 flat x86-64", "entry": "0x400000", "bytes": len(data),
                             "sha256": sha256(binary), "archive_path": archive_path, "command": command}
    archive = out / "DOS64.IMG"
    archive.write_bytes(pack_archive(files))
    after = {str(p.relative_to(ROOT)): sha256(p) for p in source_files}
    if before != after:
        raise RuntimeError("SD64 source files changed during assembly; rebuild from stable sources")
    result = {"abi": "ShizukuDOS native SD64 v1", "apps": apps,
              "archive": {"file": str(archive), "bytes": archive.stat().st_size, "sha256": sha256(archive)},
              "sources": {p.name: sha256(p) for p in source_files},
              "source_files_sha256": before,
              "artifacts_sha256": {name: apps[name]["sha256"] for name in apps} | {"DOS64.IMG": sha256(archive)}}
    (out / "build-result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "dos64")
    args = parser.parse_args()
    print(json.dumps(build(args.out.resolve()), indent=2))


if __name__ == "__main__":
    main()
