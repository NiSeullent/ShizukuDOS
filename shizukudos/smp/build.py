#!/usr/bin/env python3
"""Assemble the independent ShizukuDOS x86 SMP boot prototype floppy."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
from pathlib import Path


HERE = Path(__file__).resolve().parent
FLOPPY_BYTES = 1_474_560


def build(output: Path, nasm: str = "nasm", fault: bool = False,
          omit_done: bool = False, protected_ap: bool = False,
          omit_pm_entry: bool = False, stall_bsp: bool = False,
          omit_ap_progress: bool = False, duplicate_madt: bool = False) -> Path:
    if sum((fault, omit_done, omit_pm_entry, stall_bsp,
            omit_ap_progress, duplicate_madt)) > 1:
        raise ValueError("Only one SMP fault mode may be selected")
    if omit_pm_entry and not protected_ap:
        raise ValueError("The protected-mode entry marker exists only in the protected AP variant")
    executable = shutil.which(nasm)
    if executable is None:
        raise RuntimeError("NASM is required to build the SMP prototype")
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    binaries = {}
    for name, size in (("boot", 512), ("bsp", 8192), ("ap", 512)):
        suffix = ("-pm" if protected_ap and name != "boot" else "")
        suffix += "-fault" if fault and name == "bsp" else "-omit-done" if omit_done and name == "ap" else ""
        suffix += "-omit-pm" if omit_pm_entry and name == "ap" else ""
        # Intermediate binaries belong to this image build. Distinct image
        # variants may be assembled by independent test runners concurrently.
        target = output.parent / (output.stem + "-" + name + suffix + ".bin")
        command = [executable, "-f", "bin"]
        if protected_ap and name == "bsp":
            command.append("-DPROTECTED_AP=1")
        if stall_bsp and name == "bsp":
            command.append("-DSTALL_BSP_AFTER_FLAG=1")
        if duplicate_madt and name == "bsp":
            command.append("-DDUPLICATE_MADT=1")
        if fault and name == "bsp":
            command.append("-DFAULT_INJECT=1")
        if omit_done and name == "ap":
            command.append("-DOMIT_DONE=1")
        if omit_pm_entry and name == "ap":
            command.append("-DOMIT_PM_ENTRY=1")
        if omit_ap_progress and name == "ap":
            command.append("-DOMIT_AP_PROGRESS=1")
        source = "ap_pm.asm" if protected_ap and name == "ap" else name + ".asm"
        command += [str(HERE / source), "-o", str(target)]
        subprocess.run(command, check=True)
        data = target.read_bytes()
        if len(data) != size:
            raise RuntimeError(f"{name}.bin must be exactly {size} bytes")
        binaries[name] = data
    if binaries["boot"][510:512] != b"\x55\xaa":
        raise RuntimeError("Boot signature is absent")
    image = binaries["boot"] + binaries["bsp"] + binaries["ap"]
    output.write_bytes(image.ljust(FLOPPY_BYTES, b"\0"))
    print(f"Built {output} ({FLOPPY_BYTES} bytes, SHA-256 {hashlib.sha256(output.read_bytes()).hexdigest()})")
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=HERE / "out" / "smp.img")
    parser.add_argument("--nasm", default="nasm")
    parser.add_argument("--fault-inject", action="store_true",
                        help="build a deliberate AP-result-corruption test image")
    parser.add_argument("--omit-done", action="store_true",
                        help="build an AP-completion-timeout test image")
    parser.add_argument("--protected-ap", action="store_true",
                        help="use the opt-in protected-mode AP trampoline and BSP gate")
    parser.add_argument("--omit-pm-entry", action="store_true",
                        help="omit the AP protected-mode marker to test the BSP gate")
    parser.add_argument("--stall-bsp", action="store_true",
                        help="wait until APs finish after setting BSP_RUNNING")
    parser.add_argument("--omit-ap-progress", action="store_true",
                        help="omit AP progress publications to test BSP observation")
    parser.add_argument("--duplicate-madt", action="store_true",
                        help="synthesize a duplicate enabled xAPIC ID before IPI")
    args = parser.parse_args()
    build(args.output, args.nasm, args.fault_inject, args.omit_done,
          args.protected_ap, args.omit_pm_entry, args.stall_bsp,
          args.omit_ap_progress, args.duplicate_madt)


if __name__ == "__main__":
    main()
