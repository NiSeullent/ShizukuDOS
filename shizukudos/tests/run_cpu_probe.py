"""Boot an ephemeral ShizukuDOS floppy and exercise CPU discovery over COM1.

QEMU TCG is a development emulator check, not the project's required
VT-x/AMD-V acceptance test. No installed Windows 98 disk or VM is touched.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "build/shizukudos.img"
DEFAULT_QEMU = Path(r"C:\Program Files\qemu\qemu-system-i386.exe")


def probe(qemu: Path, cpu: str, logical: int,
          threads_per_core: int = 1) -> str:
    if logical % threads_per_core:
        raise ValueError("logical count must divide into cores and threads")
    cores = logical // threads_per_core
    command = [
        str(qemu), "-machine", "pc", "-m", "64", "-boot", "a",
        "-drive", f"file={IMAGE},format=raw,if=floppy,readonly=on",
        "-display", "none", "-monitor", "none", "-serial", "stdio",
        "-nic", "none", "-no-reboot", "-cpu", cpu,
        "-smp", f"{logical},sockets=1,cores={cores},threads={threads_per_core}",
    ]
    process = subprocess.Popen(
        command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    output = bytearray()
    lock = threading.Lock()

    def collect() -> None:
        assert process.stdout is not None
        while True:
            chunk = process.stdout.read(1)
            if not chunk:
                return
            with lock:
                output.extend(chunk)

    reader = threading.Thread(target=collect, daemon=True)
    reader.start()
    try:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            with lock:
                seen = bytes(output)
            if b"A:\\>" in seen:
                break
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited before prompt: {process.returncode}")
            time.sleep(0.05)
        else:
            raise TimeoutError("ShizukuDOS prompt did not appear")
        assert process.stdin is not None
        # The shell echoes through BIOS video before polling COM1 again;
        # sending a whole command at once can overrun the emulated 16450 RX.
        for character in b"CPU\n":
            process.stdin.write(bytes((character,)))
            process.stdin.flush()
            time.sleep(0.6)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            with lock:
                seen = bytes(output)
            if b"CPU discovery" in seen and seen.count(b"A:\\>") >= 2:
                return seen.decode("ascii", errors="replace")
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited during CPU command: {process.returncode}")
            time.sleep(0.05)
        raise TimeoutError("CPU command did not return to prompt; serial output: " +
                           seen.decode("ascii", errors="replace")[-2000:])
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
        reader.join(timeout=1)


def check_output(cpu: str, logical_expected: int,
                 threads_expected: int, output: str) -> None:
    if "CPU discovery (read-only CPUID hints):" not in output:
        raise AssertionError(f"{cpu}/{logical_expected}: no CPU discovery output")
    if cpu == "486,level=0":
        if "CPUID leaf 1 unavailable; family, topology and features unknown." not in output:
            raise AssertionError("CPUID max-basic-leaf guard failed")
        if "Family 0 " in output:
            raise AssertionError("unavailable family was presented as a real CPU")
        return
    # QEMU's 486 model exposes CPUID (family 4). Its emulator cannot prove
    # the pre-CPUID branch, which remains guarded by the EFLAGS.ID check.
    if cpu == "486" and "Family 4 " not in output:
        raise AssertionError(f"{cpu}: 486 signature decode failed")
    if cpu == "pentium" and "Family 5 " not in output:
        raise AssertionError(f"{cpu}: Pentium family decode failed")
    if "Vendor: " not in output:
        raise AssertionError(f"{cpu}/{logical_expected}: CPUID vendor absent")
    match = re.search(r"Package logical (\d+) cores ([\w]+) threads/core ([\w]+)", output)
    if not match:
        raise AssertionError(f"{cpu}/{logical_expected}: package topology line absent")
    logical = int(match.group(1))
    if logical != logical_expected:
        raise AssertionError(f"{cpu}/{logical_expected}: package logical count {logical}")
    if cpu in ("SandyBridge", "phenom", "qemu32") and (
        match.group(2) != str(logical_expected // threads_expected) or
        match.group(3) != str(threads_expected)
    ):
        raise AssertionError(f"{cpu}/{logical_expected}: core/SMT split {match.groups()}")
    if cpu == "phenom" and "Topology source: AMD leaf 80000008h" not in output:
        raise AssertionError("AMD extended core-count fallback was not used")
    features = re.search(
        r"PAE HW ([YN]) XSAVE HW ([YN]) OSXSAVE now ([YN]) "
        r"AVX HW ([YN]) AVX usable now ([YN])", output
    )
    if not features:
        raise AssertionError(f"{cpu}/{logical_expected}: feature line absent")
    pae, xsave, osxsave, avx, active = features.groups()
    if active == "Y" and (xsave, osxsave, avx) != ("Y", "Y", "Y"):
        raise AssertionError(f"{cpu}/{logical_expected}: AVX active without prerequisites")
    if cpu == "pentium" and (pae, xsave, avx) != ("N", "N", "N"):
        raise AssertionError(f"{cpu}: modern features incorrectly advertised")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", type=Path, default=DEFAULT_QEMU)
    args = parser.parse_args()
    if not args.qemu.is_file():
        parser.error(f"QEMU executable not found: {args.qemu}")
    if not IMAGE.is_file():
        parser.error(f"build the floppy first: {IMAGE}")
    for cpu, logical, threads in (
        ("486", 1, 1), ("486,level=0", 1, 1), ("pentium", 1, 1),
        ("qemu32", 1, 1), ("qemu32", 2, 1),
        ("phenom", 2, 1), ("SandyBridge", 2, 1),
        ("SandyBridge", 4, 2),
    ):
        observed = probe(args.qemu, cpu, logical, threads)
        try:
            check_output(cpu, logical, threads, observed)
        except AssertionError:
            print(observed)
            raise
        lines = [line for line in observed.splitlines()
                 if line.startswith(("CPUID", "Vendor:", "Family ",
                                     "Package ", "Topology source:", "PAE HW"))]
        print(f"PASS: {cpu} / {logical} vCPU ({threads}/core): {' | '.join(lines)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
