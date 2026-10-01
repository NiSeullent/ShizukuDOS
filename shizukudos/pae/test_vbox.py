#!/usr/bin/env python3
"""Hardware-backed PAE test in new disposable VirtualBox VMs only."""

from __future__ import annotations

import json
import hashlib
import os
import re
import subprocess
import time
import uuid
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


def vbox_path() -> Path:
    candidate = Path(os.environ.get("PROGRAMFILES", "C:/Program Files")) / "Oracle" / "VirtualBox" / "VBoxManage.exe"
    if not candidate.is_file():
        raise RuntimeError("VBoxManage.exe is required")
    return candidate


def vbox(executable: Path, *args: str) -> str:
    result = subprocess.run([str(executable), *args], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=120)
    if result.returncode:
        raise RuntimeError(f"VBoxManage {args[0]} failed: {result.stderr[-1000:]}")
    return result.stdout


def machine_info(executable: Path, name: str) -> dict[str, str]:
    info = {}
    for line in vbox(executable, "showvminfo", name, "--machinereadable").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in ("VMState", "CfgFile", "firmware", "hwvirtex", "pae", "memory"):
            try:
                info[key] = json.loads(value)
            except json.JSONDecodeError:
                info[key] = value.strip('"')
    return info


def run_one(executable: Path, image: Path, megabytes: int, timeout: int) -> str:
    name = f"Shizuku-PAE-Probe-{megabytes}-{uuid.uuid4().hex[:8]}"
    base = (HERE / "out").resolve()
    vm_dir = base / name
    serial = base / f"{name}-serial.log"
    registered = False
    try:
        vbox(executable, "createvm", "--name", name, "--basefolder", str(base),
             "--ostype", "Other", "--register")
        registered = True
        vbox(executable, "modifyvm", name, "--memory", str(megabytes),
             "--cpus", "1", "--firmware", "bios", "--chipset", "piix3",
             "--ioapic", "on", "--pae", "on", "--long-mode", "off",
             "--hwvirtex", "on", "--nested-paging", "off", "--boot1", "floppy",
             "--boot2", "none", "--boot3", "none", "--boot4", "none",
             "--nic1", "none", "--uart1", "0x3f8", "4", "--uart-mode1", "file", str(serial))
        vbox(executable, "storagectl", name, "--name", "Floppy", "--add", "floppy")
        vbox(executable, "storageattach", name, "--storagectl", "Floppy",
             "--port", "0", "--device", "0", "--type", "fdd", "--medium", str(image))
        info = machine_info(executable, name)
        if (Path(info.get("CfgFile", "")).resolve().parent != vm_dir
                or info.get("firmware", "").upper() != "BIOS" or info.get("hwvirtex") != "on"
                or info.get("pae") != "on" or int(info.get("memory", 0)) != megabytes):
            raise RuntimeError("Disposable VM failed path/BIOS/PAE/VT configuration checks")
        vbox(executable, "startvm", name, "--type", "headless")
        deadline = time.monotonic() + timeout
        transcript = ""
        while time.monotonic() < deadline:
            if serial.is_file():
                transcript = serial.read_text(encoding="latin1", errors="replace")
                if "PAE RESULT" in transcript:
                    break
            time.sleep(0.2)
        if "PAE RESULT" not in transcript:
            raise RuntimeError(f"VM {megabytes} MiB produced no result: {transcript[-800:]}")
        if megabytes >= 5120:
            match = E820.search(transcript)
            if PASS not in transcript or not match or int(match.group(1), 16) < 0x100200000:
                raise RuntimeError(f"VM {megabytes} MiB high-page proof failed: {transcript[-800:]}")
            verify_probe(transcript)
        elif LOW_ERROR not in transcript or " FAIL" not in transcript:
            raise RuntimeError(f"VM {megabytes} MiB E820 rejection failed: {transcript[-800:]}")
        log = vm_dir / "Logs" / "VBox.log"
        if not log.is_file():
            raise RuntimeError("The current VM execution log is missing")
        runtime = log.read_text(encoding="utf-8", errors="replace")
        if "NEM: Created partition" in runtime:
            backend = "WHPX/NEM"
            backend_line = next(line for line in runtime.splitlines() if "NEM: Created partition" in line)
        elif "HM: HMR3Init: VT-x w/" in runtime or "HM: HMR3Init: AMD-V w/" in runtime:
            backend = "VT-x/AMD-V"
            backend_line = next(line for line in runtime.splitlines()
                                if "HM: HMR3Init: VT-x w/" in line or "HM: HMR3Init: AMD-V w/" in line)
        else:
            raise RuntimeError("Current VM log does not prove a hardware execution backend")
        evidence = {
            "memory_mib": megabytes,
            "disposable_vm": name,
            "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
            "virtualbox_version": vbox(executable, "--version").strip(),
            "runtime_backend": backend,
            "current_vbox_log_backend_line": backend_line,
            "serial_transcript": transcript,
            "result": "high physical page verified" if megabytes >= 5120 else "E820 high page rejected",
        }
        (base / f"{name}-evidence.json").write_text(
            json.dumps(evidence, indent=2) + "\n", encoding="utf-8"
        )
        if megabytes >= 5120:
            return f"{megabytes} MiB: E820-confirmed physical 4 GiB+4 KiB write/read/restore, no low alias, {backend}; PASS"
        return f"{megabytes} MiB: E820 high-page rejection, {backend}; PASS"
    finally:
        if registered:
            try:
                info = machine_info(executable, name)
                # Cleanup is limited to the unique VM created under pae/out.
                if Path(info.get("CfgFile", "")).resolve().parent == vm_dir:
                    if info.get("VMState") in ("running", "paused", "stuck"):
                        vbox(executable, "controlvm", name, "poweroff")
                    vbox(executable, "unregistervm", name, "--delete")
            except Exception as exc:
                print(f"Cleanup needed for disposable VM {name}: {exc}")


def main() -> None:
    image = build(HERE / "out" / "pae.img")
    executable = vbox_path()
    print(run_one(executable, image, 5120, 60))
    print(run_one(executable, image, 3072, 45))


if __name__ == "__main__":
    main()
