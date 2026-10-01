#!/usr/bin/env python3
"""Boot the isolated SMP floppy under QEMU with 2 and 4 vCPUs."""

from __future__ import annotations

import argparse
from functools import lru_cache
import os
import re
import shutil
import subprocess
from pathlib import Path

from build import HERE, build


RESULT = re.compile(r"SMP RESULT bsp=0x([0-9A-F]{2}) expected=0x([0-9A-F]{2}) "
                    r"ack=0x([0-9A-F]{2}) started=0x([0-9A-F]{2}) "
                    r"done=0x([0-9A-F]{2}) overlap=0x([0-9A-F]{2}) "
                    r"failures=0x([0-9A-F]{2}) (PASS|FAIL)")
AP_WORK = re.compile(r"SMP WORK ap=0x([0-9A-F]{2}) seed=0x([0-9A-F]{8}) result=0x([0-9A-F]{8})")
BSP_WORK = re.compile(r"SMP WORK bsp=0x([0-9A-F]{2}) seed=0x([0-9A-F]{8}) result=0x([0-9A-F]{8})")
PM_ENTRY = re.compile(r"SMP PM entered=0x([0-9A-F]{2}) stack_bytes=512 range=0x30000\.\.0x50000")
PROGRESS = re.compile(r"SMP PROGRESS bsp_seen=0x([0-9A-F]{2}) ap_seen=0x([0-9A-F]{2})")


def verify_progress(transcript: str, bsp_seen: int, ap_seen: int) -> None:
    readings = PROGRESS.findall(transcript)
    if (len(readings) != 1
            or tuple(int(value, 16) for value in readings[0]) != (bsp_seen, ap_seen)):
        raise RuntimeError("Bidirectional BSP/AP work-progress observations did not match")


def verify_pm(transcript: str, expected_aps: int) -> None:
    if "SMP STAGE2: xAPIC protected-mode AP work prototype" not in transcript:
        raise RuntimeError("Missing protected-mode AP boot banner")
    entries = PM_ENTRY.findall(transcript)
    if len(entries) != 1 or int(entries[0], 16) != expected_aps:
        raise RuntimeError("Protected-mode AP entry count or stack layout was not verified")


@lru_cache(maxsize=None)
def worker_hash(seed: int, iterations: int) -> int:
    value = seed
    for index in range(iterations):
        value = (((value << 5) | (value >> 27)) & 0xffffffff) ^ 0x9e3779b9
        value = (value + index) & 0xffffffff
    return value


def verify_work(transcript: str, cpus: int) -> None:
    ap_lines = AP_WORK.findall(transcript)
    if len(ap_lines) != cpus - 1:
        raise RuntimeError("Missing or duplicate AP work result lines")
    ids = set()
    for ap_id_text, seed_text, result_text in ap_lines:
        ap_id, seed, result = (int(x, 16) for x in (ap_id_text, seed_text, result_text))
        if ap_id in ids or seed != (0x5a17c0de ^ ap_id) or result != worker_hash(seed, 2_000_000):
            raise RuntimeError("AP seed/result did not match an independent host calculation")
        ids.add(ap_id)
    bsp_lines = BSP_WORK.findall(transcript)
    if len(bsp_lines) != 1:
        raise RuntimeError("Missing BSP work result")
    bsp_id, bsp_seed, bsp_result = (int(x, 16) for x in bsp_lines[0])
    if bsp_seed != (0xb5b50000 ^ bsp_id) or bsp_result != worker_hash(bsp_seed, 16_000_000):
        raise RuntimeError("BSP result did not match an independent host calculation")


def find_qemu() -> Path:
    found = shutil.which("qemu-system-i386")
    if found:
        return Path(found)
    candidate = Path(os.environ.get("PROGRAMFILES", "C:/Program Files")) / "qemu" / "qemu-system-i386.exe"
    if candidate.is_file():
        return candidate
    raise RuntimeError("qemu-system-i386 was not found")


def run_one(qemu: Path, image: Path, cpus: int, accelerator: str, timeout: int,
            protected_ap: bool = False) -> str:
    command = [str(qemu), "-machine", "pc,acpi=on", "-accel", accelerator,
               "-cpu", "qemu32", "-m", "64", "-smp", str(cpus),
               "-drive", f"file={image},format=raw,if=floppy", "-boot", "a",
               "-snapshot", "-display", "none", "-serial", "stdio",
               "-monitor", "none", "-no-reboot",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    try:
        proc = subprocess.run(command, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout,
                              check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"QEMU -smp {cpus} timed out") from exc
    transcript = proc.stdout + proc.stderr
    match = RESULT.search(transcript)
    if not match:
        raise RuntimeError(f"QEMU -smp {cpus} had no SMP result (exit {proc.returncode}):\n{transcript[-2500:]}")
    bsp, expected, acknowledged, started, done, overlap, failures = (
        int(match.group(i), 16) for i in range(1, 8)
    )
    if (proc.returncode != 33 or match.group(8) != "PASS" or expected != cpus - 1
            or any(value != expected for value in (acknowledged, started, done, overlap))
            or failures != 0 or bsp >= cpus):
        raise RuntimeError(f"QEMU -smp {cpus} failed (exit {proc.returncode}):\n{transcript[-2500:]}")
    verify_work(transcript, cpus)
    verify_progress(transcript, cpus - 1, cpus - 1)
    if protected_ap:
        verify_pm(transcript, cpus - 1)
    return (f"-smp {cpus}: BSP {bsp}, AP ack/start/done/overlap {expected}/{expected}, "
            "per-CPU results independently verified; PASS")


def run_no_acpi(qemu: Path, image: Path, accelerator: str, timeout: int,
                protected_ap: bool = False) -> str:
    """The missing-MADT path must terminate with a specific failure, not hang."""
    command = [str(qemu), "-machine", "pc,acpi=off", "-accel", accelerator,
               "-cpu", "qemu32", "-m", "64", "-smp", "2",
               "-drive", f"file={image},format=raw,if=floppy", "-boot", "a",
               "-snapshot", "-display", "none", "-serial", "stdio",
               "-monitor", "none", "-no-reboot",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    try:
        proc = subprocess.run(command, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout,
                              check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("QEMU ACPI-negative test timed out") from exc
    transcript = proc.stdout + proc.stderr
    if (proc.returncode != 35 or "SMP ERROR: valid ACPI MADT not found" not in transcript
            or not RESULT.search(transcript) or " FAIL" not in transcript):
        raise RuntimeError(f"QEMU ACPI-negative test did not fail cleanly:\n{transcript[-2500:]}")
    verify_progress(transcript, 0, 0)
    if protected_ap:
        verify_pm(transcript, 0)
    return "-smp 2 with ACPI disabled: specific MADT error and bounded exit; PASS"


def run_bad_result(qemu: Path, image: Path, accelerator: str, timeout: int,
                   protected_ap: bool = False) -> str:
    """Fault image flips one completed AP result; BSP must reject it."""
    command = [str(qemu), "-machine", "pc,acpi=on", "-accel", accelerator,
               "-cpu", "qemu32", "-m", "64", "-smp", "2",
               "-drive", f"file={image},format=raw,if=floppy", "-boot", "a",
               "-snapshot", "-display", "none", "-serial", "stdio",
               "-monitor", "none", "-no-reboot",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    try:
        proc = subprocess.run(command, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout,
                              check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("QEMU corrupt-result test timed out") from exc
    transcript = proc.stdout + proc.stderr
    match = RESULT.search(transcript)
    if (proc.returncode != 35 or "SMP ERROR: AP result mismatch" not in transcript
            or not match or match.group(8) != "FAIL"
            or any(int(match.group(i), 16) != 1 for i in (2, 3, 4, 5, 6))
            or int(match.group(7), 16) != 0):
        raise RuntimeError(f"QEMU corrupt-result test did not reject bad work:\n{transcript[-2500:]}")
    verify_progress(transcript, 1, 1)
    if protected_ap:
        verify_pm(transcript, 1)
    return "-smp 2 with a corrupted AP result: explicit mismatch and bounded failure; PASS"


def run_missing_done(qemu: Path, image: Path, accelerator: str, timeout: int,
                     protected_ap: bool = False) -> str:
    """A finished AP deliberately omits its completion signal; BSP must time out."""
    command = [str(qemu), "-machine", "pc,acpi=on", "-accel", accelerator,
               "-cpu", "qemu32", "-m", "64", "-smp", "2",
               "-drive", f"file={image},format=raw,if=floppy", "-boot", "a",
               "-snapshot", "-display", "none", "-serial", "stdio",
               "-monitor", "none", "-no-reboot",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    try:
        proc = subprocess.run(command, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout,
                              check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("QEMU missing-completion test exceeded its host timeout") from exc
    transcript = proc.stdout + proc.stderr
    match = RESULT.search(transcript)
    if (proc.returncode != 35 or "SMP ERROR: AP work completion timeout or AP failure" not in transcript
            or not match or match.group(8) != "FAIL"
            or tuple(int(match.group(i), 16) for i in (2, 3, 4, 5, 6, 7))
            != (1, 1, 1, 0, 1, 0)):
        raise RuntimeError(f"QEMU missing-completion test did not time out cleanly:\n{transcript[-2500:]}")
    verify_progress(transcript, 1, 1)
    if protected_ap:
        verify_pm(transcript, 1)
    return "-smp 2 with a missing completion signal: BSP timeout and bounded failure; PASS"


def run_missing_pm_entry(qemu: Path, image: Path, accelerator: str, timeout: int) -> str:
    """AP completes real work, but BSP must reject its missing PM marker."""
    command = [str(qemu), "-machine", "pc,acpi=on", "-accel", accelerator,
               "-cpu", "qemu32", "-m", "64", "-smp", "2",
               "-drive", f"file={image},format=raw,if=floppy", "-boot", "a",
               "-snapshot", "-display", "none", "-serial", "stdio",
               "-monitor", "none", "-no-reboot",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    try:
        proc = subprocess.run(command, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout,
                              check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("QEMU missing-PM-entry test exceeded its host timeout") from exc
    transcript = proc.stdout + proc.stderr
    match = RESULT.search(transcript)
    if (proc.returncode != 35 or "SMP ERROR: AP protected-mode entry count mismatch" not in transcript
            or not match or match.group(8) != "FAIL"
            or tuple(int(match.group(i), 16) for i in (2, 3, 4, 5, 6, 7))
            != (1, 1, 1, 1, 1, 0)):
        raise RuntimeError(f"QEMU missing-PM-entry test did not reject bad evidence:\n{transcript[-2500:]}")
    verify_progress(transcript, 1, 1)
    verify_pm(transcript, 0)
    return "-smp 2 with a missing protected-mode marker: explicit BSP mismatch; PASS"


def run_sync_fault(qemu: Path, image: Path, accelerator: str, timeout: int,
                   kind: str, protected_ap: bool) -> str:
    """Check the synthetic preemption, AP progress, and duplicate-ID gates."""
    expected = {
        "stalled-bsp": ("SMP ERROR: APs did not overlap BSP work phase",
                        (1, 1, 1, 1, 0, 0), (0, 0), 1),
        "silent-ap": ("SMP ERROR: APs did not overlap BSP work phase",
                      (1, 1, 1, 1, 1, 0), (0, 1), 1),
        "duplicate-madt": ("SMP ERROR: unsupported or malformed MADT processors",
                           (0, 0, 0, 0, 0, 0), (0, 0), 0),
    }
    error, counters, progress, pm_entries = expected[kind]
    command = [str(qemu), "-machine", "pc,acpi=on", "-accel", accelerator,
               "-cpu", "qemu32", "-m", "64", "-smp", "2",
               "-drive", f"file={image},format=raw,if=floppy", "-boot", "a",
               "-snapshot", "-display", "none", "-serial", "stdio",
               "-monitor", "none", "-no-reboot",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    try:
        proc = subprocess.run(command, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout,
                              check=False)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"QEMU {kind} test exceeded its host timeout") from exc
    transcript = proc.stdout + proc.stderr
    match = RESULT.search(transcript)
    if (proc.returncode != 35 or error not in transcript
            or not match or match.group(8) != "FAIL"
            or tuple(int(match.group(i), 16) for i in (2, 3, 4, 5, 6, 7)) != counters):
        raise RuntimeError(f"QEMU {kind} test failed its specific gate:\n{transcript[-2500:]}")
    verify_progress(transcript, *progress)
    if protected_ap:
        verify_pm(transcript, pm_entries)
    return f"-smp 2 {kind}: specific bounded failure before an invalid PASS; PASS"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accel", default="whpx" if os.name == "nt" else "kvm",
                        choices=("whpx", "kvm", "hvf", "tcg"))
    parser.add_argument("--qemu", type=Path)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--protected-ap", action="store_true",
                        help="test the opt-in protected-mode AP worker")
    args = parser.parse_args()
    suffix = "-pm" if args.protected_ap else ""
    image = build(HERE / "out" / f"smp{suffix}.img", protected_ap=args.protected_ap)
    qemu = args.qemu or find_qemu()
    for count in (2, 4):
        print(run_one(qemu, image, count, args.accel, args.timeout, args.protected_ap))
    print(run_no_acpi(qemu, image, args.accel, args.timeout, args.protected_ap))
    fault_image = build(HERE / "out" / f"smp{suffix}-fault.img", fault=True,
                        protected_ap=args.protected_ap)
    print(run_bad_result(qemu, fault_image, args.accel, args.timeout, args.protected_ap))
    missing_done_image = build(HERE / "out" / f"smp{suffix}-omit-done.img", omit_done=True,
                               protected_ap=args.protected_ap)
    print(run_missing_done(qemu, missing_done_image, args.accel, args.timeout,
                           args.protected_ap))
    if args.protected_ap:
        missing_pm_image = build(HERE / "out" / "smp-pm-omit-entry.img",
                                 protected_ap=True, omit_pm_entry=True)
        print(run_missing_pm_entry(qemu, missing_pm_image, args.accel, args.timeout))
    stalled_image = build(HERE / "out" / f"smp{suffix}-stalled-bsp.img",
                          protected_ap=args.protected_ap, stall_bsp=True)
    print(run_sync_fault(qemu, stalled_image, args.accel, args.timeout,
                         "stalled-bsp", args.protected_ap))
    silent_ap_image = build(HERE / "out" / f"smp{suffix}-silent-ap.img",
                            protected_ap=args.protected_ap, omit_ap_progress=True)
    print(run_sync_fault(qemu, silent_ap_image, args.accel, args.timeout,
                         "silent-ap", args.protected_ap))
    duplicate_image = build(HERE / "out" / f"smp{suffix}-duplicate-madt.img",
                            protected_ap=args.protected_ap, duplicate_madt=True)
    print(run_sync_fault(qemu, duplicate_image, args.accel, args.timeout,
                         "duplicate-madt", args.protected_ap))
    if args.accel == "tcg":
        print("Software TCG test only: this does not meet the project's VT-x/AMD-V acceptance gate")
    else:
        print(f"Hardware accelerator {args.accel} tested for both configurations")


if __name__ == "__main__":
    main()
