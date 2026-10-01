#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Standalone ShizukuDOS build, test and package entry point."""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu
import shzlib

ROOT = shzlib.REPO


def invoke(relative, *args, timeout=1800):
    return subprocess.run([sys.executable, "-B", str(ROOT / relative), *map(str, args)],
                          cwd=ROOT, timeout=timeout).returncode


def doctor():
    required = ("python3", "nasm", "gcc", "ld", "objcopy", "objdump", "nm",
                "x86_64-w64-mingw32-gcc", "mformat", "mmd", "mcopy", "xorriso", "git")
    tools = {name: shutil.which(name) for name in required}
    tools.update({"qemu": qemu.DEFAULT_QEMU if Path(qemu.DEFAULT_QEMU).exists() else None,
                  "ovmf_code": qemu.DEFAULT_OVMF_CODE if Path(qemu.DEFAULT_OVMF_CODE).exists() else None,
                  "ovmf_vars": qemu.DEFAULT_OVMF_VARS if Path(qemu.DEFAULT_OVMF_VARS).exists() else None})
    print(json.dumps({"product": "ShizukuDOS 10", "tools": tools,
                      "default_track": "DOS-only: own BIOS shell + native UEFI Kernel64",
                      "default_build_downloads": False,
                      "host_configuration_changes": False}, indent=2))
    return 0 if all(tools.values()) else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("doctor", help="inspect installed build and verification tools")
    build = sub.add_parser("build", help="build an independent DOS ISO or optional research profile")
    build.add_argument("--profile", default="dos-only",
                       choices=("dos-only", "bios-legacy", "uefi-multikernel", "dual-bios-uefi-csm"))
    test = sub.add_parser("test", help="run a focused suite; save its result under build/")
    test.add_argument("--suite", choices=("host", "iso"), default="iso")
    package = sub.add_parser("package", help="package the verified DOS release and corresponding source")
    package.add_argument("--version", default="10.0.0-preview.1")
    args = ap.parse_args()
    if args.command == "doctor":
        return doctor()
    if args.command == "build":
        if args.profile == "dos-only":
            return invoke("tools/build_iso.py")
        # Opt-in upstream profiles never enter the default ISO.
        # DOS16/CSMWrap builds fetch the commits in shizukudos/upstream/manifest.json.
        if args.profile == "bios-legacy":
            return invoke("shizukudos/dos16/build.py")
        steps = [("shizukudos/dos16/build.py", ())]
        if args.profile == "dual-bios-uefi-csm":
            steps.insert(0, ("shizukudos/csm/build.py", ()))
        else:
            steps += [("shizukudos/kbuild.py", ()),
                      ("shizukudos/win64/build.py", ("--no-wineport",)),
                      ("shizukudos/supervisor/build.py", ())]
        for script, opts in steps:
            rc = invoke(script, *opts)
            if rc:
                return rc
        return 0
    if args.command == "test":
        script = "shizukudos/abi/test_abi.py" if args.suite == "host" else "tools/test_iso.py"
        rc = invoke(script)
        shzlib.write_json(ROOT / "build" / "results" / f"{args.suite}.json",
                          {"suite": args.suite, "status": "PASS" if rc == 0 else "FAIL",
                           "script": script, "exit_code": rc, "utc": shzlib.utc_now()})
        return rc
    return invoke("tools/package_release.py", "--version", args.version)


if __name__ == "__main__":
    sys.exit(main())
