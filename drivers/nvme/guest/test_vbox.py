#!/usr/bin/env python3
"""Disposable BIOS/CSM VirtualBox NVMe read-only guest test (GPL-2.0-only).

Only newly created VMs inside drivers/nvme/build/vbox are controlled. The
guest waits at NVME_READY; this harness validates VBox's assigned BAR region
before injecting Y. Without that host proof the guest never reads MMIO.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import time
import uuid
from pathlib import Path

try:
    from .build import build
except ImportError:  # direct `python guest/test_vbox.py` invocation
    from build import build


HERE = Path(__file__).resolve().parent
BASE = (HERE.parent / "build" / "vbox").resolve()
READY = re.compile(
    r"NVME_READY BDF=([0-9a-f]{8}) ID=([0-9a-f]{8}) "
    r"CLASS=([0-9a-f]{8}) CMD=([0-9a-f]{8}) BAR=([0-9a-f]{8})",
    re.IGNORECASE,
)
MMIO = re.compile(r"NVME_MMIO CAP=([0-9a-f]{8}):([0-9a-f]{8}) "
                  r"VS=([0-9a-f]{8})", re.IGNORECASE)
DEVICE = re.compile(r"(?m)^([0-9a-f]{2}):([0-9a-f]{2})\.([0-7]) "
                    r"nvme: ([0-9a-f]{4})-([0-9a-f]{4})\b", re.IGNORECASE)
NEXT_DEVICE = re.compile(r"(?m)^[0-9a-f]{2}:[0-9a-f]{2}\.[0-7] ", re.IGNORECASE)
REGION = re.compile(r"MMIO(?:32|64)(?: PREFETCH)? region #0: "
                    r"([0-9a-f]+)\.\.([0-9a-f]+)", re.IGNORECASE)


def vbox_path() -> Path:
    path = Path(os.environ.get("VBOX_MSI_INSTALL_PATH",
                               r"C:\Program Files\Oracle\VirtualBox")) / "VBoxManage.exe"
    if not path.is_file():
        raise RuntimeError("VBoxManage.exe is required")
    return path


def vbox(executable: Path, *args: str) -> str:
    result = subprocess.run([str(executable), *args], capture_output=True,
                            text=True, encoding="utf-8", errors="replace",
                            timeout=120)
    if result.returncode:
        raise RuntimeError(f"VBoxManage {args[0]} failed: {result.stderr[-1200:]}")
    return result.stdout


def machine_info(executable: Path, name: str) -> dict[str, str]:
    values = {}
    for line in vbox(executable, "showvminfo", name, "--machinereadable").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in ("VMState", "CfgFile", "firmware", "hwvirtex"):
            try:
                values[key] = json.loads(value)
            except json.JSONDecodeError:
                values[key] = value.strip('"')
    return values


def wait_for(serial: Path, pattern: re.Pattern[str], timeout: int = 35) -> tuple[str, re.Match[str]]:
    deadline = time.monotonic() + timeout
    transcript = ""
    while time.monotonic() < deadline:
        if serial.is_file():
            transcript = serial.read_text(encoding="latin1", errors="replace")
            match = pattern.search(transcript)
            if match:
                return transcript, match
        time.sleep(0.2)
    raise RuntimeError(f"guest serial timeout for {pattern.pattern}: {transcript[-700:]}")


def confirm_vbox_region(debug_pci: str, ready: re.Match[str]) -> dict[str, int | str]:
    bdf, pci_id, class_rev, command, bar = (int(value, 16) for value in ready.groups())
    bus, device, function = bdf >> 8, (bdf & 0xff) >> 3, bdf & 7
    if (class_rev >> 8) != 0x010802 or not (command & 2) or not bar:
        raise RuntimeError("guest PCI class, memory decode, or BAR is invalid")
    for match in DEVICE.finditer(debug_pci):
        found = tuple(int(match.group(i), 16) for i in range(1, 6))
        if found[:3] != (bus, device, function):
            continue
        vendor, product = found[3:]
        if (vendor, product) != (0x80EE, 0x4E56) or pci_id != (product << 16 | vendor):
            raise RuntimeError("guest PCI ID does not match the VirtualBox NVMe model")
        next_device = NEXT_DEVICE.search(debug_pci, match.end())
        section = debug_pci[match.end():next_device.start() if next_device else None]
        region = REGION.search(section)
        if not region:
            raise RuntimeError("VBox PCI debugger did not expose NVMe BAR0 length")
        start, last = (int(value, 16) for value in region.groups())
        length = last - start + 1
        if (start != bar or last < start or length < 12 or
                length > (16 << 20) or length & (length - 1) or
                start + length > (1 << 32)):
            raise RuntimeError("guest BAR and VBox-assigned MMIO region disagree")
        return {"bdf": f"{bus:02x}:{device:02x}.{function}",
                "vendor_id": vendor, "device_id": product,
                "bar_base": start, "bar_bytes_host_reported": length,
                "bar_region_line": region.group(0)}
    raise RuntimeError("VBox PCI debugger did not list the guest-discovered NVMe BDF")


def backend_from_log(log: Path) -> tuple[str, str]:
    if not log.is_file():
        raise RuntimeError("current disposable VM has no VBox.log")
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    for marker, label in (("NEM: Created partition", "WHPX/NEM hardware"),
                          ("HM: HMR3Init: VT-x w/", "VT-x hardware"),
                          ("HM: HMR3Init: AMD-V w/", "AMD-V hardware")):
        match = next((line for line in lines if marker in line), None)
        if match:
            return label, match
    raise RuntimeError("current VBox.log does not prove hardware acceleration")


def run_case(executable: Path, image: Path, case: str,
             controller: bool, approve: bool) -> dict[str, object]:
    token = uuid.uuid4().hex[:8]
    name = f"Shizuku-NVMe-{case}-{token}"
    vm_dir = BASE / name
    serial = BASE / f"{name}-serial.log"
    registered = False
    try:
        vbox(executable, "createvm", "--name", name, "--basefolder", str(BASE),
             "--ostype", "Other", "--register")
        registered = True
        vbox(executable, "modifyvm", name, "--memory", "64", "--cpus", "1",
             "--firmware", "bios", "--chipset", "piix3", "--hwvirtex", "on",
             "--boot1", "floppy", "--boot2", "none", "--boot3", "none",
             "--boot4", "none", "--nic1", "none", "--uart1", "0x3f8", "4",
             "--uart-mode1", "file", str(serial))
        vbox(executable, "storagectl", name, "--name", "Floppy", "--add", "floppy")
        vbox(executable, "storageattach", name, "--storagectl", "Floppy",
             "--port", "0", "--device", "0", "--type", "fdd", "--medium", str(image))
        if controller:
            # A controller with no namespace medium suffices for CAP/VS reads.
            vbox(executable, "storagectl", name, "--name", "NVMe", "--add",
                 "pcie", "--controller", "NVMe")
        info = machine_info(executable, name)
        if (Path(info.get("CfgFile", "")).resolve().parent != vm_dir or
                info.get("firmware", "").upper() != "BIOS" or
                info.get("hwvirtex") != "on"):
            raise RuntimeError("new VM failed path/BIOS/VT preflight")
        vbox(executable, "startvm", name, "--type", "headless")
        host_region = None
        debug_section = ""
        if controller:
            _, ready = wait_for(serial, READY)
            if approve:
                debug_section = vbox(executable, "debugvm", name, "info", "pci")
                host_region = confirm_vbox_region(debug_section, ready)
            vbox(executable, "controlvm", name, "keyboardputstring",
                 "Y" if approve else "N")
            expected = (re.compile(r"NVME_RESULT PASS") if approve else
                        re.compile(r"NVME_RESULT FAIL HOST_DENIED"))
            transcript, _ = wait_for(serial, expected)
            if approve:
                mmio = MMIO.search(transcript)
                if not mmio:
                    raise RuntimeError("guest did not report CAP/VS")
                cap_hi, cap_lo, version = (int(value, 16) for value in mmio.groups())
                if (cap_lo & 0xffff) == 0 or version in (0, 0xffffffff):
                    raise RuntimeError("guest NVMe CAP/VS is invalid")
            elif "NVME_MMIO" in transcript:
                raise RuntimeError("host-denied guest unexpectedly read MMIO")
        else:
            transcript, _ = wait_for(serial,
                                     re.compile(r"NVME_RESULT FAIL NO_DEVICE"))
            if "NVME_MMIO" in transcript:
                raise RuntimeError("no-controller guest unexpectedly read MMIO")
        backend, backend_line = backend_from_log(vm_dir / "Logs" / "VBox.log")
        evidence = {
            "case": case, "disposable_vm": name, "firmware": info["firmware"],
            "nvme_controller_attached": controller, "mmio_host_approved": approve,
            "virtualbox_version": vbox(executable, "--version").strip(),
            "floppy_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
            "runtime_backend": backend,
            "current_vbox_log_backend_line": backend_line,
            "host_pci_region": host_region,
            "serial_transcript": transcript,
        }
        evidence_path = BASE / f"{name}-evidence.json"
        evidence_path.write_text(json.dumps(evidence, indent=2) + "\n",
                                 encoding="utf-8")
        print(f"{case}: {backend}; "
              f"BAR bytes {host_region['bar_bytes_host_reported'] if host_region else 'none'}; "
              f"guest {'PASS' if approve else 'safe negative path'}")
        return evidence
    finally:
        if registered:
            # Never power off or unregister a VM unless its current config is
            # exactly the newly created directory below our ignored build/.
            try:
                info = machine_info(executable, name)
                if Path(info.get("CfgFile", "")).resolve().parent != vm_dir:
                    raise RuntimeError("refusing unsafe VM cleanup path")
                if info.get("VMState") in ("running", "paused", "stuck"):
                    vbox(executable, "controlvm", name, "poweroff")
                vbox(executable, "unregistervm", name, "--delete")
            except Exception as exc:
                print(f"Manual cleanup needed for {name}: {exc}")


def main() -> None:
    BASE.mkdir(parents=True, exist_ok=True)
    image = build(HERE.parent / "build" / "nvme-bios-probe.img")
    executable = vbox_path()
    run_case(executable, image, "positive", controller=True, approve=True)
    run_case(executable, image, "host-denied", controller=True, approve=False)
    run_case(executable, image, "no-controller", controller=False, approve=False)


if __name__ == "__main__":
    main()
