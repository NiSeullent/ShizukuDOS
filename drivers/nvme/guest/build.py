#!/usr/bin/env python3
"""Build the independent BIOS floppy NVMe observation image (GPL-2.0-only)."""

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
    for name, expected in (("boot", 512), ("stage", 8192)):
        target = output.parent / f"nvme-guest-{name}.bin"
        subprocess.run([executable, "-f", "bin", str(HERE / f"{name}.asm"),
                        "-o", str(target)], check=True)
        data = target.read_bytes()
        if len(data) != expected:
            raise RuntimeError(f"{name} length {len(data)} != {expected}")
        parts.append(data)
    if parts[0][510:512] != b"\x55\xaa":
        raise RuntimeError("BIOS boot signature is absent")
    image = b"".join(parts).ljust(FLOPPY_BYTES, b"\0")
    output.write_bytes(image)
    print(f"Built {output} ({FLOPPY_BYTES} bytes, SHA-256 "
          f"{hashlib.sha256(image).hexdigest()})")
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=HERE.parent / "build" / "nvme-bios-probe.img")
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    build(args.output, args.nasm)


if __name__ == "__main__":
    main()
