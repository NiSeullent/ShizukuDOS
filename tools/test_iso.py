#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inspect and boot the actual DOS-only ISO under isolated QEMU BIOS and UEFI."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def firmware_pair():
    for directory, suffix in (("/usr/share/edk2/ovmf", ""), ("/usr/share/OVMF", "_4M"), ("/usr/share/OVMF", "")):
        code, variables = Path(directory) / f"OVMF_CODE{suffix}.fd", Path(directory) / f"OVMF_VARS{suffix}.fd"
        if code.is_file() and variables.is_file():
            return code, variables
    return Path("/usr/share/edk2/ovmf/OVMF_CODE.fd"), Path("/usr/share/edk2/ovmf/OVMF_VARS.fd")


def check(name, ok, detail=""):
    return {"check": name, "status": "PASS" if ok else "FAIL", "detail": detail}


def layout(iso, out):
    report = subprocess.run(["xorriso", "-indev", str(iso), "-report_el_torito", "plain"],
                            capture_output=True, text=True, check=True)
    text = report.stdout + report.stderr
    (out / "el-torito.txt").write_text(text)
    checks = [check("El Torito catalog has BIOS floppy-emulation boot", bool(re.search(r"boot img\s*:\s*\d+\s+BIOS\s+y\s+fd1\.4", text)), text[-2500:]),
              check("El Torito catalog has UEFI no-emulation boot", bool(re.search(r"boot img\s*:\s*\d+\s+UEFI\s+y\s+none", text)))]
    extract = out / "extracted"
    if extract.exists(): shutil.rmtree(extract)
    subprocess.run(["xorriso", "-osirrox", "on", "-indev", str(iso), "-extract", "/", str(extract)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    manifest = json.loads((extract / "MANIFEST.json").read_text())
    actual_files = {str(path.relative_to(extract)) for path in extract.rglob("*") if path.is_file()}
    expected_files = set(manifest["files"]) | {"MANIFEST.json", "BOOT/BOOT.CAT"}
    checks.append(check("ISO file set matches its manifest exactly", actual_files == expected_files,
                        "unexpected=" + repr(sorted(actual_files - expected_files)) + "; missing=" + repr(sorted(expected_files - actual_files))))
    for name, record in manifest["files"].items():
        path = extract / name
        checks.append(check("ISO content hash: " + name, path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == record["sha256"]))
    checks.append(check("ISO includes corresponding source and licensing", all((extract / name).is_file() for name in
                        ("SOURCE/SHIZUKUDOS-SOURCE.tar.gz", "SOURCE/LICENSE", "SOURCE/THIRD_PARTY.md"))))
    with tarfile.open(extract / "SOURCE/SHIZUKUDOS-SOURCE.tar.gz", "r:gz") as sources:
        entries = sources.getmembers()
        members = {m.name: m for m in entries if m.isfile()}
        expected_sources = {"ShizukuDOS/" + name for name in manifest["source_hashes"]}
        source_valid = len(entries) == len(members) and set(members) == expected_sources and all("ShizukuDOS/" + name in members and
                           hashlib.sha256(sources.extractfile(members["ShizukuDOS/" + name]).read()).hexdigest() == digest
                           for name, digest in manifest["source_hashes"].items())
    checks.append(check("ISO corresponding source file set and hashes match exactly", source_valid))
    return checks, manifest


def base_command(qemu, iso):
    return [str(qemu), "-machine", "pc", "-accel", "tcg", "-cpu", "max", "-m", "256", "-cdrom", str(iso),
            "-boot", "d", "-display", "none", "-monitor", "none", "-nic", "none", "-no-reboot"]


def bios(qemu, iso, out, timeout):
    command = base_command(qemu, iso) + ["-serial", "stdio"]
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    buffer, lock = bytearray(), threading.Lock()

    def collect():
        while True:
            chunk = proc.stdout.read(1)
            if not chunk: return
            with lock: buffer.extend(chunk)

    reader = threading.Thread(target=collect, daemon=True)
    reader.start()
    checks = []
    deadline = time.monotonic() + timeout

    def seen():
        with lock: return bytes(buffer).decode("ascii", errors="replace")

    def wait_for(predicate):
        while time.monotonic() < deadline and proc.poll() is None:
            if predicate(seen()): return True
            time.sleep(0.05)
        return predicate(seen())

    try:
        ready = wait_for(lambda text: "A:\\>" in text)
        checks.append(check("BIOS ISO boot reaches original DOS16 shell", ready))
        if ready:
            for cmd, marker in (("DIR", "DEMO    .COM"), ("TYPE HELLO.TXT", "Hello from the independent ShizukuDOS ISO."),
                                ("EXEC DEMO.COM", "INT 21h version/error checks OK")):
                prompts = seen().count("A:\\>")
                for character in (cmd + "\n").encode("ascii"):
                    proc.stdin.write(bytes((character,)))
                    proc.stdin.flush()
                    # The real-mode shell polls an unbuffered UART between BIOS calls.
                    time.sleep(0.15)
                passed = wait_for(lambda text: marker in text and text.count("A:\\>") > prompts)
                checks.append(check("BIOS DOS16 command: " + cmd, passed, "expected " + marker))
                if not passed: break
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=3)
        reader.join(timeout=1)
    serial = seen()
    (out / "bios-serial.log").write_text(serial)
    return {"command": command, "checks": checks, "returncode": proc.returncode,
            "qemu_output": proc.stderr.read().decode(errors="replace"), "serial": "bios-serial.log"}


def uefi(qemu, iso, out, code, vars_path, timeout):
    fresh_vars = out / "OVMF_VARS.fd"
    shutil.copyfile(vars_path, fresh_vars)
    serial_path = out / "uefi-serial.log"
    serial_path.unlink(missing_ok=True)
    command = base_command(qemu, iso) + ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
              "-drive", f"if=pflash,format=raw,unit=1,file={fresh_vars}", "-serial", f"file:{serial_path}",
              "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    timed_out = False
    try: proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.terminate()
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=3)
    serial = serial_path.read_text(errors="replace") if serial_path.exists() else ""
    checks = [check("UEFI ISO boot exits firmware boot services", "DOS-UEFI: ExitBootServices PASS; entering native Kernel64." in serial),
              check("UEFI ISO starts native Kernel64 without VMX", "started directly by the UEFI boot manager (no Supervisor)" in serial),
              check("UEFI kernel receives GOP framebuffer", "UEFI GOP framebuffer" in serial),
              check("native HELLO64.SD64 completes", "DOS64: HELLO64.SD64 exit=00000000 faulted=0 timeout=0 reaped=0" in serial),
              check("native HELLO64 arithmetic uses 64-bit result", "HELLO64: 100000000 * 100000000 = 0x002386f26fc10000 (10000000000000000); 64-bit arithmetic PASS" in serial),
              check("native MEM64.SD64 completes", "DOS64: MEM64.SD64 exit=00000000 faulted=0 timeout=0 reaped=0" in serial),
              check("native MEM64 uses memory above 4 GiB and verifies checksum", "MEM64: virtual base=0x0000000200000000 bytes=65536 checksum=0x46688aacc1fff000; verify and release PASS" in serial),
              check("native samples report zero failures", "DOS64: completed 2 application(s), 0 failure(s)" in serial),
              check("guest exits successfully within deadline", not timed_out and bool(re.search(r"^SHZ-EXIT:0\s*$", serial, re.M)) and proc.returncode == 1,
                    f"qemu_rc={proc.returncode}, timeout={timed_out}")]
    return {"command": command, "checks": checks, "returncode": proc.returncode,
            "qemu_output": proc.stdout.read().decode(errors="replace"), "serial": "uefi-serial.log"}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--iso", type=Path, default=REPO / "build/shizukudos-10-dos-only.iso")
    p.add_argument("--qemu", type=Path, default=Path(shutil.which("qemu-system-x86_64") or "/usr/libexec/qemu-kvm"))
    default_code, default_vars = firmware_pair()
    p.add_argument("--ovmf-code", type=Path, default=default_code)
    p.add_argument("--ovmf-vars", type=Path, default=default_vars)
    p.add_argument("--timeout", type=int, default=180)
    p.add_argument("--out", type=Path, default=REPO / "build/iso-tests")
    p.add_argument("--layout-only", action="store_true")
    args = p.parse_args()
    if not args.iso.is_file(): p.error("ISO not found: " + str(args.iso))
    if not args.layout_only:
        for path in (args.qemu, args.ovmf_code, args.ovmf_vars):
            if not path.is_file(): p.error("test dependency not found: " + str(path))
    args.out.mkdir(parents=True, exist_ok=True)
    layout_checks, manifest = layout(args.iso, args.out)
    result = {"iso": str(args.iso), "iso_sha256": hashlib.sha256(args.iso.read_bytes()).hexdigest(),
              "kernel_sha256": manifest["kernel_sha256"], "initrd_sha256": manifest["initrd_sha256"],
              "source_hashes": manifest["source_hashes"], "payload_hashes": manifest["files"],
              "inputs_verified": manifest.get("inputs_verified", False),
              "manifest_sha256": hashlib.sha256((args.out / "extracted/MANIFEST.json").read_bytes()).hexdigest(),
              "layout": layout_checks, "profiles": {}}
    if not args.layout_only:
        # A single guest at a time; no NIC, host disk, USB passthrough or installed OS.
        print("Booting BIOS DOS16 from the actual ISO", flush=True)
        result["profiles"]["bios"] = bios(args.qemu, args.iso, args.out, args.timeout)
        print("Booting UEFI64 native DOS apps from the actual ISO", flush=True)
        result["profiles"]["uefi"] = uefi(args.qemu, args.iso, args.out, args.ovmf_code, args.ovmf_vars, args.timeout)
    all_checks = result["layout"] + [c for profile in result["profiles"].values() for c in profile["checks"]]
    successful = all(c["status"] == "PASS" for c in all_checks)
    result["status"] = ("LAYOUT_ONLY" if args.layout_only else "PASS") if successful else "FAIL"
    result["verification"] = "layout only" if args.layout_only else "actual BIOS and UEFI ISO boots under QEMU TCG"
    (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    for c in all_checks: print(f"[{c['status']}] {c['check']}")
    print(result["status"])
    return 0 if successful else 1


if __name__ == "__main__":
    sys.exit(main())
