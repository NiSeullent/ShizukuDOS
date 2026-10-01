#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inspect and boot the actual DOS-only ISO under isolated QEMU BIOS and UEFI."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import socket
import struct
import subprocess
import sys
import tarfile
import threading
import time
import wave
import zlib
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


def archive_files(data):
    if len(data) < 16 or data[:8] != b"SHZARC01":
        raise ValueError("invalid native archive header")
    count, reserved = struct.unpack_from("<II", data, 8)
    end = 16 + count * 136
    if not count or count > 256 or reserved or end > len(data):
        raise ValueError("invalid native archive table")
    result, ranges = {}, []
    for i in range(count):
        entry = 16 + i * 136
        raw = data[entry:entry + 120]
        name = raw.split(b"\0", 1)[0].decode("ascii")
        offset, length = struct.unpack_from("<QQ", data, entry + 120)
        if (b"\0" not in raw or not name.startswith("\\") or name in result
                or offset < end or offset % 16 or offset > len(data) or length > len(data) - offset
                or any(offset < stop and start < offset + length for start, stop in ranges)):
            raise ValueError("invalid or overlapping native archive entry: " + name)
        result[name] = data[offset:offset + length]
        ranges.append((offset, offset + length))
    return result


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
    embedded = archive_files((extract / "SAMPLES/DOS64.IMG").read_bytes())
    manifest["verified_initrd_files"] = {name: hashlib.sha256(data).hexdigest() for name, data in embedded.items()}
    for name in manifest["files"]:
        if name.startswith("SAMPLES/") and name.endswith((".EXE", ".SD64")):
            filename = Path(name).name
            matches = [data for path, data in embedded.items() if path.rsplit("\\", 1)[-1] == filename]
            checks.append(check("distributed application equals executed initrd bytes: " + filename,
                                len(matches) == 1 and matches[0] == (extract / name).read_bytes()))
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


class QMP:
    """One bounded local QEMU control connection; no network listener."""
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect(str(path))
        self.stream = self.sock.makefile("rwb", buffering=0)
        self.serial = 0
        json.loads(self.stream.readline())
        self.call("qmp_capabilities")

    def call(self, command, arguments=None):
        self.serial += 1
        request = {"execute": command, "id": self.serial}
        if arguments is not None:
            request["arguments"] = arguments
        self.stream.write(json.dumps(request).encode() + b"\n")
        while True:
            response = json.loads(self.stream.readline())
            if response.get("id") == self.serial:
                if "error" in response:
                    raise RuntimeError(response["error"])
                return response.get("return")

    def key(self, name):
        self.call("human-monitor-command", {"command-line": "sendkey " + name})

    def close(self):
        self.stream.close()
        self.sock.close()


def screenshot(qmp, path):
    """Convert QEMU's P6 screenshot to PNG with only the Python standard library."""
    ppm = path.with_suffix(".ppm")
    qmp.call("screendump", {"filename": str(ppm)})
    magic, dimensions, maximum, rgb = ppm.read_bytes().split(b"\n", 3)
    width, height = map(int, dimensions.split())
    if magic != b"P6" or maximum != b"255" or len(rgb) != width * height * 3:
        raise RuntimeError("unexpected QEMU framebuffer screenshot")
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    ppm.unlink()
    return {"file": path.name, "width": width, "height": height,
            "colors": len({rgb[i:i + 3] for i in range(0, len(rgb), 3)}),
            "sha256": hashlib.sha256(rgb).hexdigest()}


def uefi(qemu, iso, out, code, vars_path, timeout):
    fresh_vars = out / "OVMF_VARS.fd"
    shutil.copyfile(vars_path, fresh_vars)
    serial_path = out / "uefi-serial.log"
    serial_path.unlink(missing_ok=True)
    qmp_path = out / "qmp.sock"
    qmp_path.unlink(missing_ok=True)
    audio_path = out / "speaker.wav"
    audio_path.unlink(missing_ok=True)
    # Some distribution builds omit PC speaker emulation. Record that explicitly;
    # stock QEMU also verifies the emitted signal rather than just speaker register writes.
    machine_help = subprocess.run([str(qemu), "-machine", "pc,help"], capture_output=True, text=True, check=True)
    speaker_available = "pcspk-audiodev" in machine_help.stdout
    command = base_command(qemu, iso)
    if speaker_available:
        command[command.index("pc")] = "pc,pcspk-audiodev=speaker"
        command += ["-audiodev", f"wav,id=speaker,path={audio_path}"]
    command += ["-qmp", f"unix:{qmp_path},server=on,wait=off",
              "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
              "-drive", f"if=pflash,format=raw,unit=1,file={fresh_vars}", "-serial", f"file:{serial_path}",
              "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    timed_out, control_error = False, ""
    shots, qmp = [], None
    deadline = time.monotonic() + timeout
    def seen():
        return serial_path.read_text(errors="replace") if serial_path.exists() else ""
    def wait_frame(key, pane, url=None):
        offset = len(seen())
        qmp.key(key)
        frame_deadline = min(deadline, time.monotonic() + 20)
        while proc.poll() is None and time.monotonic() < frame_deadline:
            text = seen()[offset:]
            # QEMU TCG can spend longer than a fixed sleep presenting GOP MMIO.
            # A frame is captured only after input handling and actual presentation.
            input_at = text.find("SHZGUI INPUT ")
            if input_at >= 0:
                frames = re.findall(r"SHZGUI PRESENT pane=(\d+) seq=\d+ url=([^\r\n]+)", text[input_at:])
                if any(int(p) == pane and (url is None or path == url) for p, path in frames):
                    return
            time.sleep(0.05)
        raise RuntimeError(f"keyboard {key} did not complete a GOP frame for pane {pane}, url={url}")
    try:
        while proc.poll() is None and time.monotonic() < deadline:
            if "SHZGUI INTERACTIVE ready" in seen():
                qmp = QMP(qmp_path)
                shots.append(screenshot(qmp, out / "desktop.png"))
                for name, key, pane in (("browser", "f1", 3), ("video", "f2", 1), ("music", "f3", 2), ("thread-tree", "f4", 4), ("utilities", "f5", 5)):
                    wait_frame(key, pane)
                    shots.append(screenshot(qmp, out / (name + ".png")))
                wait_frame("ret", 5)  # execute the selected HELLO64 from the GUI
                for _ in range(14):
                    wait_frame("tab", 5)
                wait_frame("ret", 5)  # HELLO.EXE: actual Real Mode and return
                for _ in range(3):
                    wait_frame("tab", 5)
                wait_frame("ret", 5)  # HELLO32.EXE: Protected Mode and return
                wait_frame("f1", 3)
                wait_frame("tab", 3)
                wait_frame("ret", 3, "C:\\WWW\\MODES.HTM")
                shots.append(screenshot(qmp, out / "browser-follow.png"))
                wait_frame("backspace", 3, "C:\\WWW\\INDEX.HTM")
                wait_frame("f2", 1)
                wait_frame("spc", 1)
                wait_frame("spc", 1)
                wait_frame("f3", 2)
                wait_frame("spc", 2)
                time.sleep(1)
                wait_frame("spc", 2)
                qmp.key("f10")
                break
            time.sleep(0.05)
        remaining = max(0.1, deadline - time.monotonic())
        proc.wait(timeout=remaining)
    except subprocess.TimeoutExpired:
        timed_out = True
    except (OSError, ValueError, RuntimeError) as exc:
        control_error = str(exc)
    finally:
        if qmp:
            qmp.close()
        if proc.poll() is None:
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
              check("native samples report zero failures", bool(re.search(r"DOS64: completed [1-9][0-9]* application\(s\), 0 failure\(s\)", serial))),
              check("formal UEFI startup accepts all native modes and apps", "SHZ-NATIVE-ACCEPT: PASS failures=0" in serial),
              check("CPU executes actual Real Mode", bool(re.search(r"MODES: REAL16 CR0=[0-9a-f]+ EFER=[0-9a-f]+ CS=[0-9a-f]+ PASS", serial))),
              check("CPU executes actual 32-bit Protected Mode", bool(re.search(r"MODES: PROTECTED32 CR0=[0-9a-f]+ EFER=[0-9a-f]+ CS=[0-9a-f]+ PASS", serial))),
              check("CPU returns to actual Long Mode", bool(re.search(r"MODES: LONG64 .*return PASS", serial))),
              check("all six native legacy examples finish successfully", "MODES: completed 6 native application(s), 0 failure(s)" in serial),
              check("GOP software GUI self-test passes", "SHZGUI SELFTEST PASS" in serial),
              check("native PE loader rejects malformed/dependent images", "NATIVE64: malformed-image/dependency rejection 6 checks, 0 failure(s)" in serial),
              check("original HTTP browser fetches through native TCP loopback", "SHZGUI HTTP PASS" in serial),
              check("GOP GUI remains interactive after startup", "SHZGUI READY" in serial and len(shots) == 7, control_error),
              check("rendered GOP framebuffer contains true-color pixels", len(shots) >= 3 and shots[0]["colors"] > 256 and shots[2]["colors"] > 256, str(shots)),
              check("keyboard selects visibly different windows", len({s["sha256"] for s in shots[1:6]}) == 5),
              check("guest exits successfully within deadline", not timed_out and bool(re.search(r"^SHZ-EXIT:0\s*$", serial, re.M)) and proc.returncode == 1,
                    f"qemu_rc={proc.returncode}, timeout={timed_out}")]
    manifest = json.loads((out / "extracted/MANIFEST.json").read_text())
    native_exes = sorted(Path(name).name for name in manifest["files"] if name.startswith("SAMPLES/") and name.endswith("64.EXE"))
    checks.append(check("ISO ships at least ten real native 64-bit EXEs", len(native_exes) >= 10, str(native_exes)))
    for name in native_exes:
        checks.append(check("native PE32+ application completes: " + name,
                            f"DOS64: {name} exit=00000000 faulted=0 timeout=0 reaped=0" in serial))
    checks.append(check("keyboard activates all five GUI panes", all(f"pane={n}" in serial for n in range(1, 6))))
    interactive = serial.partition("SHZGUI INTERACTIVE ready")[2]
    checks.append(check("keyboard follows a packaged browser hyperlink", "SHZGUI BROWSER path=C:\\WWW\\MODES.HTM" in interactive
                        and "SHZGUI INPUT scan=1c pane=3" in interactive))
    checks.append(check("keyboard returns through browser history", "SHZGUI BROWSER path=C:\\WWW\\INDEX.HTM" in interactive))
    for name in ("HELLO64.EXE", "HELLO.EXE", "HELLO32.EXE"):
        checks.append(check("GUI launches and returns from application: " + name, f"SHZGUI LAUNCH {name} result=0" in interactive))
    audio = {"backend": "QEMU PC speaker WAV" if speaker_available else "PC speaker omitted by this QEMU build",
             "signal_verified": False}
    if speaker_available:
        try:
            with wave.open(str(audio_path), "rb") as wav:
                frames = wav.readframes(wav.getnframes())
                audio.update({"frames": wav.getnframes(), "rate": wav.getframerate(), "channels": wav.getnchannels(),
                              "signal_verified": len(set(frames)) > 2})
        except (wave.Error, OSError) as exc:
            audio["error"] = str(exc)
        checks.append(check("music player emits a non-silent PC speaker signal", audio["signal_verified"], str(audio)))
    return {"command": command, "checks": checks, "returncode": proc.returncode,
            "qemu_output": proc.stdout.read().decode(errors="replace"), "serial": "uefi-serial.log",
            "screenshots": shots, "audio": audio}


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
              "initrd_files": manifest["verified_initrd_files"],
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
