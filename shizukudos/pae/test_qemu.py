#!/usr/bin/env python3
"""Exercise high-RAM PAE and low-RAM failure paths in QEMU."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
from pathlib import Path

from build import HERE, build


PASS = "PAE RESULT physical=0x0000000100001000 alias=clear restored=yes PASS"
LOW_ERROR = "PAE ERROR: no E820 type-1 span covers 4 GiB + 2 MiB"
E820 = re.compile(r"PAE E820: target=0x0000000100001000 usable_end=0x([0-9A-F]{16})")
PROBE = re.compile(r"PAE PROBE original=0x([0-9A-F]{8}) pattern=0x([0-9A-F]{8}) "
                   r"observed=0x([0-9A-F]{8}) low_before=0x([0-9A-F]{8}) "
                   r"low_during=0x([0-9A-F]{8}) restored=0x([0-9A-F]{8})")


def verify_probe(transcript: str) -> None:
    match = PROBE.search(transcript)
    if not match or "PAE PAGING: CR0.PG=1 CR4.PAE=1 PDE.phys_hi=1" not in transcript:
        raise RuntimeError("PAE probe did not report active paging and measured dwords")
    original, pattern, observed, low_before, low_during, restored = (
        int(match.group(i), 16) for i in range(1, 7)
    )
    if (pattern != (original ^ 0xA55A96C3) or observed != pattern
            or low_during != low_before or restored != original):
        raise RuntimeError("PAE dword measurement or restoration is inconsistent")


def find_qemu() -> Path:
    found = shutil.which("qemu-system-i386")
    if found:
        return Path(found)
    path = Path(os.environ.get("PROGRAMFILES", "C:/Program Files")) / "qemu" / "qemu-system-i386.exe"
    if path.is_file():
        return path
    raise RuntimeError("qemu-system-i386 was not found")


def run_one(qemu: Path, image: Path, megabytes: int, accelerator: str,
            timeout: int) -> str:
    command = [str(qemu), "-machine", "pc,acpi=on", "-accel", accelerator,
               "-cpu", "qemu32", "-m", str(megabytes), "-smp", "1",
               "-drive", f"file={image},format=raw,if=floppy", "-boot", "a",
               "-snapshot", "-display", "none", "-serial", "stdio",
               "-monitor", "none", "-no-reboot",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    try:
        proc = subprocess.run(command, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout,
                              check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"QEMU {megabytes} MiB timed out") from exc
    transcript = proc.stdout + proc.stderr
    if megabytes >= 5120:
        match = E820.search(transcript)
        if proc.returncode != 33 or PASS not in transcript or not match:
            raise RuntimeError(f"QEMU {megabytes} MiB high-RAM probe failed:\n{transcript[-2500:]}")
        end = int(match.group(1), 16)
        if end < 0x100200000:
            raise RuntimeError("E820 upper bound did not cover the tested 2 MiB page")
        verify_probe(transcript)
        return f"{megabytes} MiB: physical 0x100001000 read/write/restore, no 0x1000 alias; PASS"
    if proc.returncode != 35 or LOW_ERROR not in transcript or " FAIL" not in transcript:
        raise RuntimeError(f"QEMU {megabytes} MiB low-RAM path failed:\n{transcript[-2500:]}")
    return f"{megabytes} MiB: E820 correctly rejects unavailable high RAM; PASS"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accel", default="whpx" if os.name == "nt" else "kvm",
                        choices=("whpx", "kvm", "hvf", "tcg"))
    parser.add_argument("--qemu", type=Path)
    parser.add_argument("--timeout", type=int, default=75)
    args = parser.parse_args()
    image = build(HERE / "out" / "pae.img")
    qemu = args.qemu or find_qemu()
    print(run_one(qemu, image, 5120, args.accel, args.timeout))
    print(run_one(qemu, image, 3072, args.accel, args.timeout))
    if args.accel == "tcg":
        print("QEMU TCG is software-emulated; hardware acceleration must be verified separately")
    else:
        print(f"QEMU {args.accel} hardware accelerator tested")


if __name__ == "__main__":
    main()
