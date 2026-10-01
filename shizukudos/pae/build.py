#!/usr/bin/env python3
"""Build a standalone 1.44 MB BIOS floppy for the ShizukuDOS PAE experiment."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
from pathlib import Path


HERE = Path(__file__).resolve().parent
FLOPPY_BYTES = 1_474_560


def build(output: Path, nasm: str = "nasm") -> Path:
    executable = shutil.which(nasm)
    if executable is None:
        raise RuntimeError("NASM is required")
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    parts = []
    for name, size in (("boot", 512), ("stage", 8192)):
        path = output.parent / (name + ".bin")
        subprocess.run([executable, "-f", "bin", str(HERE / (name + ".asm")),
                        "-o", str(path)], check=True)
        data = path.read_bytes()
        if len(data) != size:
            raise RuntimeError(f"{name}.bin must be {size} bytes")
        parts.append(data)
    if parts[0][510:512] != b"\x55\xaa":
        raise RuntimeError("Boot signature missing")
    image = b"".join(parts).ljust(FLOPPY_BYTES, b"\0")
    output.write_bytes(image)
    print(f"Built {output} ({len(image)} bytes, SHA-256 {hashlib.sha256(image).hexdigest()})")
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=HERE / "out" / "pae.img")
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    build(args.output, args.nasm)


if __name__ == "__main__":
    main()
