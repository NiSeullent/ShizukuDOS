#!/usr/bin/env python3
"""Optional hardware-backed SMP smoke test using disposable VirtualBox VMs.

Only new VMs under smp/out are created. Existing Win98 VMs are never opened.
"""

from __future__ import annotations

import argparse
import json
import hashlib
import os
import subprocess
import time
import uuid
from pathlib import Path

from build import HERE, build
from test_qemu import PM_ENTRY, PROGRESS, RESULT, verify_pm, verify_progress, verify_work


def vbox_path() -> Path:
    path = Path(os.environ.get("PROGRAMFILES", "C:/Program Files")) / "Oracle" / "VirtualBox" / "VBoxManage.exe"
    if not path.is_file():
        raise RuntimeError("VBoxManage.exe is required")
    return path


def vbox(executable: Path, *args: str) -> str:
    result = subprocess.run([str(executable), *args], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=120)
    if result.returncode:
        raise RuntimeError(f"VBoxManage {args[0]} failed: {result.stderr[-1200:]}")
    return result.stdout


def machine_info(executable: Path, name: str) -> dict[str, str]:
    info = {}
    for line in vbox(executable, "showvminfo", name, "--machinereadable").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in ("VMState", "CfgFile", "firmware", "hwvirtex"):
            try:
                info[key] = json.loads(value)
            except json.JSONDecodeError:
                info[key] = value.strip('"')
    return info


def run_one(executable: Path, image: Path, cpus: int, timeout: int,
            expect_fault: bool = False, expect_timeout: bool = False,
            protected_ap: bool = False, expect_pm_mismatch: bool = False,
            expect_sync_fault: str | None = None) -> str:
    if sum((expect_fault, expect_timeout, expect_pm_mismatch,
            expect_sync_fault is not None)) > 1:
        raise ValueError("Only one failure mode may be selected")
    if expect_pm_mismatch and not protected_ap:
        raise ValueError("PM marker mismatch requires the protected AP variant")
    token = uuid.uuid4().hex[:8]
    name = f"Shizuku-SMP-Probe-{cpus}-{token}"
    base = (HERE / "out").resolve()
    vm_dir = base / name
    serial = base / f"{name}-serial.log"
    registered = False
    try:
        vbox(executable, "createvm", "--name", name, "--basefolder", str(base),
             "--ostype", "Other", "--register")
        registered = True
        vbox(executable, "modifyvm", name, "--memory", "64", "--cpus", str(cpus),
             "--firmware", "bios", "--chipset", "piix3", "--ioapic", "on",
             "--hwvirtex", "on", "--nested-paging", "off", "--boot1", "floppy",
             "--boot2", "none", "--boot3", "none", "--boot4", "none",
             "--nic1", "none", "--uart1", "0x3f8", "4", "--uart-mode1", "file", str(serial))
        vbox(executable, "storagectl", name, "--name", "Floppy", "--add", "floppy")
        vbox(executable, "storageattach", name, "--storagectl", "Floppy",
             "--port", "0", "--device", "0", "--type", "fdd", "--medium", str(image))
        info = machine_info(executable, name)
        if (Path(info.get("CfgFile", "")).resolve().parent != vm_dir
                or info.get("firmware", "").upper() != "BIOS" or info.get("hwvirtex") != "on"):
            raise RuntimeError("Disposable VM failed path/BIOS/VT configuration checks")
        vbox(executable, "startvm", name, "--type", "headless")
        deadline = time.monotonic() + timeout
        transcript = ""
        match = None
        while time.monotonic() < deadline:
            if serial.is_file():
                transcript = serial.read_text(encoding="latin1", errors="replace")
                match = RESULT.search(transcript)
                if (match and PROGRESS.search(transcript)
                        and (not protected_ap or PM_ENTRY.search(transcript))):
                    break
            time.sleep(0.2)
        if not match:
            raise RuntimeError(f"VM {cpus} CPUs had no AP result: {transcript[-600:]}")
        bsp, expected, ack, started, done, overlap, failures = (
            int(match.group(i), 16) for i in range(1, 8)
        )
        if protected_ap:
            verify_pm(transcript, 0 if expect_pm_mismatch or expect_sync_fault == "duplicate-madt"
                      else cpus - 1)
        if expect_sync_fault:
            scenarios = {
                "stalled-bsp": ("SMP ERROR: APs did not overlap BSP work phase",
                                (1, 1, 1, 1, 0, 0), (0, 0)),
                "silent-ap": ("SMP ERROR: APs did not overlap BSP work phase",
                              (1, 1, 1, 1, 1, 0), (0, 1)),
                "duplicate-madt": ("SMP ERROR: unsupported or malformed MADT processors",
                                   (0, 0, 0, 0, 0, 0), (0, 0)),
            }
            error, counters, progress = scenarios[expect_sync_fault]
            if (tuple(int(match.group(i), 16) for i in (2, 3, 4, 5, 6, 7)) != counters
                    or match.group(8) != "FAIL" or error not in transcript):
                raise RuntimeError(f"VM {cpus} CPUs did not reject {expect_sync_fault}: {transcript[-800:]}")
            verify_progress(transcript, *progress)
        elif expect_pm_mismatch:
            if ((expected, ack, started, done, overlap, failures)
                    != (cpus - 1, cpus - 1, cpus - 1, cpus - 1, cpus - 1, 0)
                    or match.group(8) != "FAIL"
                    or "SMP ERROR: AP protected-mode entry count mismatch" not in transcript):
                raise RuntimeError(f"VM {cpus} CPUs did not reject the PM marker mismatch: {transcript[-800:]}")
            verify_progress(transcript, cpus - 1, cpus - 1)
        elif expect_timeout:
            if ((expected, ack, started, done, overlap, failures)
                    != (cpus - 1, cpus - 1, cpus - 1, 0, cpus - 1, 0)
                    or match.group(8) != "FAIL"
                    or "SMP ERROR: AP work completion timeout or AP failure" not in transcript):
                raise RuntimeError(f"VM {cpus} CPUs did not time out cleanly: {transcript[-800:]}")
            verify_progress(transcript, cpus - 1, cpus - 1)
        elif (expected != cpus - 1
                or any(value != expected for value in (ack, started, done, overlap))
                or failures != 0):
            raise RuntimeError(f"VM {cpus} CPUs failed AP synchronization: {match.group(0)}")
        elif expect_fault:
            if match.group(8) != "FAIL" or "SMP ERROR: AP result mismatch" not in transcript:
                raise RuntimeError(f"VM {cpus} CPUs did not reject corrupted AP work: {transcript[-800:]}")
            verify_progress(transcript, cpus - 1, cpus - 1)
        else:
            if match.group(8) != "PASS":
                raise RuntimeError(f"VM {cpus} CPUs failed AP work: {transcript[-800:]}")
            verify_work(transcript, cpus)
            verify_progress(transcript, cpus - 1, cpus - 1)
        log = vm_dir / "Logs" / "VBox.log"
        if not log.is_file():
            raise RuntimeError("No current VirtualBox execution log was produced")
        runtime = log.read_text(encoding="utf-8", errors="replace")
        if "NEM: Created partition" in runtime:
            backend = "WHPX/NEM hardware"
            backend_line = next(line for line in runtime.splitlines() if "NEM: Created partition" in line)
        elif "HM: HMR3Init: VT-x w/" in runtime or "HM: HMR3Init: AMD-V w/" in runtime:
            backend = "VT-x/AMD-V hardware"
            backend_line = next(line for line in runtime.splitlines()
                                if "HM: HMR3Init: VT-x w/" in line or "HM: HMR3Init: AMD-V w/" in line)
        else:
            raise RuntimeError("The current VM log does not prove a hardware acceleration backend")
        evidence = {
            "cpus": cpus,
            "disposable_vm": name,
            "fault_injected": expect_fault,
            "completion_omitted": expect_timeout,
            "pm_entry_omitted": expect_pm_mismatch,
            "sync_fault": expect_sync_fault,
            "protected_ap": protected_ap,
            "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
            "virtualbox_version": vbox(executable, "--version").strip(),
            "runtime_backend": backend,
            "current_vbox_log_backend_line": backend_line,
            "serial_transcript": transcript,
        }
        (base / f"{name}-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
        if expect_timeout:
            return f"{cpus} CPUs: missing AP completion caused BSP timeout, {backend}; PASS"
        if expect_pm_mismatch:
            return f"{cpus} CPUs: missing AP protected-mode marker rejected, {backend}; PASS"
        if expect_sync_fault:
            return f"{cpus} CPUs: {expect_sync_fault} rejected, {backend}; PASS"
        if expect_fault:
            return f"{cpus} CPUs: corrupt AP result rejected, ack/start/done/overlap {expected}/{expected}, {backend}; PASS"
        return (f"{cpus} CPUs: BSP {bsp}, AP ack/start/done/overlap {expected}/{expected}, "
                f"per-CPU results verified, {backend}; PASS")
    finally:
        if registered:
            # The unique VM name and verified base path limit cleanup to VMs
            # created by this test. Do not touch any installed Win98 VM.
            try:
                info = machine_info(executable, name)
                if Path(info.get("CfgFile", "")).resolve().parent == vm_dir:
                    if info.get("VMState") in ("running", "paused", "stuck"):
                        vbox(executable, "controlvm", name, "poweroff")
                    vbox(executable, "unregistervm", name, "--delete")
            except Exception as exc:
                print(f"Cleanup needed for disposable VM {name}: {exc}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protected-ap", action="store_true",
                        help="test the opt-in protected-mode AP worker")
    args = parser.parse_args()
    suffix = "-pm" if args.protected_ap else ""
    image = build(HERE / "out" / f"smp{suffix}.img", protected_ap=args.protected_ap)
    executable = vbox_path()
    for cpus in (2, 4):
        print(run_one(executable, image, cpus, 45, protected_ap=args.protected_ap))
    fault_image = build(HERE / "out" / f"smp{suffix}-fault.img", fault=True,
                        protected_ap=args.protected_ap)
    print(run_one(executable, fault_image, 2, 45, expect_fault=True,
                  protected_ap=args.protected_ap))
    missing_done_image = build(HERE / "out" / f"smp{suffix}-omit-done.img", omit_done=True,
                               protected_ap=args.protected_ap)
    print(run_one(executable, missing_done_image, 2, 45, expect_timeout=True,
                  protected_ap=args.protected_ap))
    if args.protected_ap:
        missing_pm_image = build(HERE / "out" / "smp-pm-omit-entry.img",
                                 protected_ap=True, omit_pm_entry=True)
        print(run_one(executable, missing_pm_image, 2, 45,
                      protected_ap=True, expect_pm_mismatch=True))
    for fault_kind, build_flag in (("stalled-bsp", "stall_bsp"),
                                   ("silent-ap", "omit_ap_progress"),
                                   ("duplicate-madt", "duplicate_madt")):
        image = build(HERE / "out" / f"smp{suffix}-{fault_kind}.img",
                      protected_ap=args.protected_ap, **{build_flag: True})
        print(run_one(executable, image, 2, 45, protected_ap=args.protected_ap,
                      expect_sync_fault=fault_kind))


if __name__ == "__main__":
    main()
