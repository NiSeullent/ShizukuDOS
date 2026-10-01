#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bounded, isolated QEMU checks of SD64 execution and fault/timeout containment.

Build Kernel64 first with python3 shizukudos/kbuild.py. This test uses the
standalone Multiboot stub; separate ISO tests verify the UEFI boot loader.
No NIC, guest disk, monitor, VNC listener, or persistent virtual machine is used.
"""
import argparse
import json
import re
import shutil
import subprocess
import time
from pathlib import Path

from build import ROOT, build, pack_archive, sha256


def evaluate(serial, rc, case, host_timeout):
    expected = {"positive": ("00000000", 0, 0), "fault": ("c000001d", 1, 0),
                "timeout": ("00000102", 1, 1)}[case]
    code, faulted, timed_out = expected
    failures = 0 if case == "positive" else 1
    checks = {
        "bounded_host_run": not host_timeout,
        "native_hello_result": f"DOS64: HELLO64.SD64 exit={code} faulted={faulted} timeout={timed_out} reaped=0" in serial,
        "second_application_runs_after_first": "DOS64: MEM64.SD64 exit=00000000 faulted=0 timeout=0 reaped=0" in serial,
        "memory_above_4GiB_and_checksum": "MEM64: virtual base=0x0000000200000000 bytes=65536 checksum=0x46688aacc1fff000; verify and release PASS" in serial,
        "aggregate_failure_status": f"DOS64: completed 2 application(s), {failures} failure(s)" in serial,
        "guest_exit_status": bool(re.search(rf"^SHZ-EXIT:0*{failures}$", serial, re.M)),
        "qemu_exit_status": rc == (1 if failures == 0 else 3),
        "no_kernel_panic": "PANIC" not in serial,
    }
    if case == "positive":
        checks["full_64bit_product"] = "HELLO64: 100000000 * 100000000 = 0x002386f26fc10000 (10000000000000000); 64-bit arithmetic PASS" in serial
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default=shutil.which("qemu-system-x86_64") or "/usr/libexec/qemu-kvm")
    parser.add_argument("--case", choices=("all", "positive", "fault", "timeout"), default="all")
    parser.add_argument("--host-timeout", type=int, default=45)
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "dos64" / "validation")
    args = parser.parse_args()
    if not shutil.which(args.qemu):
        raise SystemExit(f"required tool missing: {args.qemu}")
    kernel_dir = ROOT / "build" / "shizukudos" / "kernel64s"
    kernel, stub = kernel_dir / "KERNEL64S.BIN", kernel_dir / "boot.elf"
    for path in (kernel, stub):
        if not path.exists():
            raise SystemExit(f"missing {path}: build shizukudos/kbuild.py first")
    samples = args.out.resolve() / "samples"
    build(samples)
    hello, memory = (samples / "HELLO64.SD64").read_bytes(), (samples / "MEM64.SD64").read_bytes()
    cases = ("positive", "fault", "timeout") if args.case == "all" else (args.case,)
    results = []
    for case in cases:
        out = args.out.resolve() / case
        out.mkdir(parents=True, exist_ok=True)
        # Deliberately different ring-3 instructions test that the kernel, not
        # a canned result reporter, obtains and contains each execution result.
        app = hello if case == "positive" else b"\x0f\x0b" if case == "fault" else b"\xeb\xfe"
        archive = out / "DOS64.IMG"
        archive.write_bytes(pack_archive([("\\SHZ\\DOS64\\HELLO64.SD64", app),
                                          ("\\SHZ\\DOS64\\MEM64.SD64", memory)]))
        serial_path = out / "serial.log"
        serial_path.unlink(missing_ok=True)
        command = [args.qemu, "-machine", "pc", "-accel", "tcg", "-cpu", "max", "-m", "256",
                   "-nodefaults", "-nic", "none", "-display", "none", "-monitor", "none",
                   "-kernel", str(stub), "-initrd", f"{kernel},{archive}", "-append", "shz.dos64=1",
                   "-serial", f"file:{serial_path}", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                   "-no-reboot"]
        start, host_timeout = time.monotonic(), False
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            output, _ = proc.communicate(timeout=args.host_timeout)
        except subprocess.TimeoutExpired:
            host_timeout = True
            proc.kill()
            output, _ = proc.communicate()
        serial = serial_path.read_text(errors="replace") if serial_path.exists() else ""
        checks = evaluate(serial, proc.returncode, case, host_timeout)
        result = {"case": case, "status": "PASS" if all(checks.values()) else "FAIL", "checks": checks,
                  "profile": "native SD64, QEMU TCG, standalone Multiboot, no network or disk",
                  "seconds": round(time.monotonic() - start, 3), "command": command,
                  "qemu_exit": proc.returncode, "qemu_output": output.decode(errors="replace"),
                  "kernel_sha256": sha256(kernel), "stub_sha256": sha256(stub),
                  "archive_sha256": sha256(archive), "serial_log": str(serial_path)}
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        results.append(result)
        print(f"{case}: {result['status']} ({result['seconds']} s)", flush=True)
        for name, passed in checks.items():
            if not passed:
                print(f"  FAIL: {name}", flush=True)
    summary = {"status": "PASS" if all(r["status"] == "PASS" for r in results) else "FAIL", "results": results}
    (args.out.resolve() / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
