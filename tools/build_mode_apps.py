#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build original DOS MZ16 and static PE32/i386 compatibility examples offline."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "samples/modes"
OUTPUT = ROOT / "build/modes"
APPS = [("hello", "HELLO.EXE", 16), ("mode", "MODE.EXE", 16), ("count", "COUNT.EXE", 16),
        ("hello32", "HELLO32.EXE", 32), ("mode32", "MODE32.EXE", 32), ("count32", "COUNT32.EXE", 32)]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    sources = [p for p in sorted(SOURCE.iterdir()) if p.is_file()]
    sources.append(Path(__file__).resolve())
    initial = {str(p.relative_to(ROOT)): sha(p) for p in sources}
    artifacts = []
    for stem, name, bits in APPS:
        output = OUTPUT / name
        subprocess.run(["nasm", "-f", "bin", "-w+all", "-I", str(SOURCE) + "/", "-o", str(output),
                        str(SOURCE / (stem + ".asm"))], check=True)
        artifacts.append({"name": name, "bits": bits, "format": "DOS MZ" if bits == 16 else "PE32/i386",
                          "path": str(output.relative_to(ROOT)), "archive_path": "\\SHZ\\MODES\\" + name,
                          "bytes": output.stat().st_size, "sha256": sha(output)})
    if initial != {str(p.relative_to(ROOT)): sha(p) for p in sources}:
        raise RuntimeError("Mode app sources changed during compilation")
    receipt = {"sources_sha256": initial, "artifacts": artifacts}
    (OUTPUT / "build-result.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
