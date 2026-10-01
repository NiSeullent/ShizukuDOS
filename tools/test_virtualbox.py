#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual VirtualBox 7 EFI32/EFI64 DVD boots in isolated, bounded, diskless VMs.

Requires an already installed, usable VirtualBox. This tool never installs or
loads host drivers. VBOX_USER_HOME and all VM files belong to its ignored output
folder. No NIC, host disk, shared folder, USB passthrough or Guest Additions.
Official CLI reference: https://www.virtualbox.org/manual/ch08.html
Exit 0=actual boots PASS (or capability available in probe-only mode);
exit 1=guest/test failure; 2=host capability unavailable.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import time
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAPABILITY_ERRORS = re.compile(r"VERR_(?:VMX_NO_VMX|VMX_MSR_ALL_VMX_DISABLED|VMX_IN_VMX_ROOT_MODE|SVM_NO_SVM|SVM_DISABLED|NEM_NOT_AVAILABLE|SUPDRV_COMPONENT_NOT_FOUND|VM_DRIVER_NOT_INSTALLED|VM_DRIVER_VERSION_MISMATCH)|vboxdrv.*(?:not loaded|not installed)|Kernel driver not installed|VT-x is not available|AMD-V is not available", re.I)
APPS = ("HELLO64", "MEM64", "TIME64", "DIR64", "TYPE64", "HASH64", "INFO64", "THREAD64", "TREE64", "CHECK64", "CPU64", "VIDEO64", "MUSIC64", "BROWSE64")


def check(name, passed, detail=""):
    return {"check": name, "status": "PASS" if passed else "FAIL", "detail": detail}


def capability(vbox):
    report = {"system": platform.platform(), "executable": str(vbox) if vbox else None, "status": "AVAILABLE"}
    if not vbox:
        return report | {"status": "UNAVAILABLE", "reason": "VBoxManage is not installed; no host installation attempted"}
    try:
        p = subprocess.run([str(vbox), "--version"], capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return report | {"status": "UNAVAILABLE", "reason": str(exc)}
    report.update(version=p.stdout.strip(), version_output=p.stderr)
    if p.returncode or not re.match(r"7\.", p.stdout.strip()):
        return report | {"status": "UNAVAILABLE", "reason": "A working VirtualBox 7.x executable is required"}
    if platform.system() == "Linux":
        cpuinfo = Path("/proc/cpuinfo").read_text(errors="replace")
        report["nested_virtualization_flag"] = bool(re.search(r"\b(?:vmx|svm)\b", cpuinfo))
        report["vboxdrv_accessible"] = os.access("/dev/vboxdrv", os.R_OK | os.W_OK)
        if not report["nested_virtualization_flag"] or not report["vboxdrv_accessible"]:
            return report | {"status": "UNAVAILABLE", "reason": "Host does not expose usable VT-x/AMD-V and /dev/vboxdrv; no host changes attempted"}
    return report


class VBox:
    def __init__(self, executable, home, transcript):
        self.executable, self.transcript = str(executable), transcript
        self.env = {**os.environ, "VBOX_USER_HOME": str(home)}
        home.mkdir(parents=True, exist_ok=True)

    def call(self, *args, require=True, timeout=20):
        command = [self.executable, *map(str, args)]
        p = subprocess.run(command, env=self.env, capture_output=True, text=True, timeout=timeout)
        with self.transcript.open("a") as out:
            out.write(json.dumps({"command": command, "returncode": p.returncode, "stdout": p.stdout, "stderr": p.stderr}) + "\n")
        if require and p.returncode:
            raise RuntimeError(p.stdout + p.stderr)
        return p

    def state(self, vm):
        p = self.call("showvminfo", vm, "--machinereadable", require=False)
        m = re.search(r'^VMState="([^"]+)"', p.stdout, re.M)
        return m.group(1) if m else "unknown"

    def key(self, vm, make):
        self.call("controlvm", vm, "keyboardputscancode", f"{make:02x}", f"{make | 0x80:02x}")


def png_info(path):
    """Validate captured RGB/RGBA pixels and record a bounded color count."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("VirtualBox screenshot is not PNG")
    at, packed, header = 8, bytearray(), None
    while at + 12 <= len(data):
        n = struct.unpack_from(">I", data, at)[0]; kind = data[at+4:at+8]; chunk = data[at+8:at+8+n]
        if at+12+n > len(data) or zlib.crc32(kind+chunk) != struct.unpack_from(">I", data, at+8+n)[0]:
            raise ValueError("Invalid PNG chunk")
        if kind == b"IHDR": header = struct.unpack(">IIBBBBB", chunk)
        if kind == b"IDAT": packed.extend(chunk)
        at += n+12
    if not header: raise ValueError("Missing PNG dimensions")
    w, h, depth, color, compression, filtering, interlace = header
    if depth != 8 or color not in (2,6) or compression or filtering or interlace or not (1 <= w <= 4096 and 1 <= h <= 4096):
        raise ValueError("Unsupported or oversized PNG screenshot")
    bpp = 3 if color == 2 else 4; stride = w*bpp; raw = zlib.decompress(packed)
    if len(raw) != (stride+1)*h: raise ValueError("Invalid PNG pixel length")
    previous = bytearray(stride); colors = set()
    for y in range(h):
        start = y*(stride+1); mode = raw[start]; row = bytearray(raw[start+1:start+1+stride])
        for x in range(stride):
            left = row[x-bpp] if x>=bpp else 0; up = previous[x]; upper_left = previous[x-bpp] if x>=bpp else 0
            if mode == 1: row[x] = (row[x]+left)&255
            elif mode == 2: row[x] = (row[x]+up)&255
            elif mode == 3: row[x] = (row[x]+((left+up)//2))&255
            elif mode == 4:
                p = left+up-upper_left; a,b,c = abs(p-left),abs(p-up),abs(p-upper_left)
                row[x] = (row[x]+(left if a<=b and a<=c else up if b<=c else upper_left))&255
            elif mode != 0: raise ValueError("Invalid PNG row filter")
        if len(colors) <= 256:
            colors.update(bytes(row[x:x+3]) for x in range(0,stride,bpp))
        previous = row
    return {"file": path.name, "width": w, "height": h, "colors_at_least": len(colors), "sha256": hashlib.sha256(data).hexdigest()}


def boot(executable, iso, out, firmware, seconds):
    folder = out/firmware; folder.mkdir(parents=True, exist_ok=True)
    serial = folder/"serial.log"; serial.unlink(missing_ok=True)
    vm = str(uuid.uuid4()); tool = VBox(executable, folder/"config", folder/"commands.jsonl")
    created, error, unavailable, shots = False, "", False, []
    started = time.monotonic(); deadline = started+seconds
    def seen(): return serial.read_text(errors="replace") if serial.exists() else ""
    def wait(predicate):
        while time.monotonic()<deadline:
            if predicate(seen()): return True
            if tool.state(vm) not in ("running","starting"): return predicate(seen())
            time.sleep(0.15)
        return predicate(seen())
    try:
        tool.call("createvm", "--name", "ShizukuDOS-UEFI-"+vm, "--uuid", vm, "--ostype", "Other_64", "--basefolder", folder/"vms", "--register")
        created = True
        tool.call("modifyvm", vm, "--memory", "256", "--cpus", "1", "--firmware", firmware, "--longmode", "on", "--ioapic", "on", "--acpi", "on", "--paravirtprovider", "none", "--graphicscontroller", "vboxvga", "--vram", "32", "--accelerate3d", "off", "--boot1", "dvd", "--boot2", "none", "--boot3", "none", "--boot4", "none", "--clipboard", "disabled", "--draganddrop", "disabled", "--usb", "off", "--usbehci", "off", "--usbxhci", "off", "--uart1", "0x3F8", "4", "--uartmode1", "file", serial,
                  *[value for n in range(1,9) for value in (f"--nic{n}", "none")])
        tool.call("storagectl", vm, "--name", "ISO-only-SATA", "--add", "sata", "--controller", "IntelAhci", "--portcount", "1", "--bootable", "on")
        tool.call("storageattach", vm, "--storagectl", "ISO-only-SATA", "--port", "0", "--device", "0", "--type", "dvddrive", "--medium", iso)
        tool.call("showvminfo", vm, "--machinereadable")
        tool.call("startvm", vm, "--type", "headless", timeout=30)
        if not wait(lambda s: "SHZGUI INTERACTIVE ready" in s): raise RuntimeError("Guest did not reach the interactive GOP desktop before the deadline")
        # F8 is an OS validation request, never a firmware boot-selection key.
        before = len(seen()); tool.key(vm, 0x42)
        if not wait(lambda s: "SHZ-NATIVE-ACCEPT: PASS failures=0" in s[before:]): raise RuntimeError("F8 native acceptance did not complete successfully")
        for name, scan, pane in (("desktop",0x3b,3),("video",0x3c,1),("music",0x3d,2),("thread-tree",0x3e,4),("utilities",0x3f,5)):
            baseline = max([int(n) for n in re.findall(r"SHZGUI PRESENT pane=\d+ seq=(\d+)", seen())] or [0])
            tool.key(vm, scan)
            if not wait(lambda s: any(int(p)==pane and int(q)>baseline for p,q in re.findall(r"SHZGUI PRESENT pane=(\d+) seq=(\d+)",s))): raise RuntimeError("GUI pane presentation was not acknowledged: "+name)
            picture = folder/(name+".png"); tool.call("controlvm", vm, "screenshotpng", picture)
            shots.append(png_info(picture))
        tool.key(vm,0x44)
        if not wait(lambda s: bool(re.search(r"^SHZ-EXIT:0\s*$",s,re.M))): raise RuntimeError("F10 did not produce a successful guest exit")
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        error = str(exc); unavailable = bool(CAPABILITY_ERRORS.search(error))
        if created:
            try:
                if tool.state(vm)=="running": tool.call("controlvm",vm,"screenshotpng",folder/"failure.png",require=False)
            except (OSError,subprocess.TimeoutExpired): pass
    finally:
        # Only our UUID is stopped/unregistered; there is no enumerate-all cleanup.
        if created:
            try:
                if tool.state(vm) in ("running","paused","stuck","starting"): tool.call("controlvm",vm,"poweroff",require=False)
                for path in (folder/"vms").rglob("VBox.log*"):
                    if path.is_file(): shutil.copy2(path,folder/path.name)
                tool.call("unregistervm",vm,"--delete",require=False)
            except (OSError,subprocess.TimeoutExpired,RuntimeError) as exc:
                error += "\nOwned VM cleanup failed: "+str(exc)
    text = seen()
    host_diagnostics = error + "\n" + "\n".join(p.read_text(errors="replace") for p in folder.glob("VBox.log*"))
    unavailable = unavailable or bool(CAPABILITY_ERRORS.search(host_diagnostics))
    checks = [check("Actual VirtualBox "+firmware+" DVD boot reaches desktop", "SHZGUI INTERACTIVE ready" in text),
              check("EFI ExitBootServices succeeds", "DOS-UEFI: ExitBootServices PASS; entering native Kernel64." in text),
              check("All native mode and application checks pass after interactive F8", "SHZ-NATIVE-ACCEPT: PASS failures=0" in text),
              check("Six real/protected-mode applications succeed", "MODES: completed 6 native application(s), 0 failure(s)" in text),
              check("All 16 long-mode programs complete without failures", "DOS64: completed 16 application(s), 0 failure(s)" in text),
              check("Compatibility SD64 applications also execute successfully", all(f"DOS64: {app}.SD64 exit=00000000 faulted=0 timeout=0 reaped=0" in text for app in ("HELLO64","MEM64"))),
              check("Actual long-mode thread ancestry, ownership, cancellation and joins pass", "THREAD64: real scheduled parent/child/grandchild, owner check, subtree cancellation + join PASS" in text),
              check("Native loader rejects malformed/dependent PE images", "NATIVE64: malformed-image/dependency rejection 6 checks, 0 failure(s)" in text),
              check("GOP GUI and original HTTP loopback pass", "SHZGUI SELFTEST PASS" in text and "SHZGUI HTTP PASS" in text),
              check("Captured five valid presented GUI panes", len(shots)==5,str(shots)),
              check("Desktop and video demonstrate more than 256 colors", all(any(p["file"]==name+".png" and p["colors_at_least"]>256 for p in shots) for name in ("desktop","video"))),
              check("Actual pane screenshots differ", len({p["sha256"] for p in shots})==5),
              check("F10 exits successfully", bool(re.search(r"^SHZ-EXIT:0\s*$",text,re.M))),
              check("Bounded run has no harness/guest error", not error,error)]
    for app in APPS:
        checks.append(check("Native PE32+ completes: "+app, f"DOS64: {app}.EXE exit=00000000 faulted=0 timeout=0 reaped=0" in text))
    result = {"firmware":firmware,"status":"UNAVAILABLE" if unavailable else "PASS" if all(c["status"]=="PASS" for c in checks) else "FAIL", "seconds":round(time.monotonic()-started,3),"checks":checks,"error":error,"vm_uuid":vm,"serial":"serial.log","screenshots":shots,"guest_contract":"256 MiB, one x64 CPU, SATA AHCI DVD only, no NIC or host disk"}
    (folder/"result.json").write_text(json.dumps(result,indent=2)+"\n")
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--iso",type=Path,default=ROOT/"build/shizukudos-10-dos-only.iso")
    p.add_argument("--vboxmanage",default=shutil.which("VBoxManage"))
    p.add_argument("--out",type=Path,default=ROOT/"build/virtualbox-tests")
    p.add_argument("--timeout",type=int,default=180)
    p.add_argument("--firmware",choices=("all","efi64","efi32"),default="all")
    p.add_argument("--probe-only",action="store_true")
    args=p.parse_args();args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=True)
    if not 15<=args.timeout<=600: p.error("timeout must be 15..600 seconds")
    host=capability(args.vboxmanage)
    result={"schema":"shizukudos.virtualbox.v1","host":host,"profiles":{}}
    if host["status"]=="UNAVAILABLE": result["status"]="UNAVAILABLE"
    elif args.probe_only: result["status"]="HOST_CAPABILITY_ONLY"
    else:
        args.iso=args.iso.resolve()
        if not args.iso.is_file(): p.error("ISO is missing: "+str(args.iso))
        result.update(iso=str(args.iso),iso_sha256=hashlib.sha256(args.iso.read_bytes()).hexdigest())
        for firmware in ("efi64","efi32") if args.firmware=="all" else (args.firmware,):
            print("Booting actual VirtualBox "+firmware+" from "+str(args.iso),flush=True)
            result["profiles"][firmware]=boot(args.vboxmanage,args.iso,args.out,firmware,args.timeout)
        statuses=[q["status"] for q in result["profiles"].values()]
        result["status"]="PASS" if statuses and all(s=="PASS" for s in statuses) else "UNAVAILABLE" if "UNAVAILABLE" in statuses else "FAIL"
    (args.out/"result.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({"status":result["status"],"host":host,"profiles":{k:v["status"] for k,v in result["profiles"].items()}},indent=2))
    return 0 if result["status"] in ("PASS","HOST_CAPABILITY_ONLY") else 2 if result["status"]=="UNAVAILABLE" else 1
if __name__=="__main__":
    raise SystemExit(main())
