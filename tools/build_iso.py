#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the independent ShizukuDOS optical-media track from this repository only."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_ISO = REPO / "build/shizukudos-10-dos-only.iso"


def run(command):
    command = [str(x) for x in command]
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=REPO, check=True, env={**os.environ, "MTOOLS_SKIP_CHECK": "1"})


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def source_manifest():
    omitted = {".git", "build", "__pycache__", ".pytest_cache"}
    paths = []
    for directory, folders, files in os.walk(REPO):
        folders[:] = [name for name in folders if name not in omitted and not (Path(directory) / name).is_symlink()]
        paths.extend(Path(directory) / name for name in files if not (Path(directory) / name).is_symlink())
    return {str(path.relative_to(REPO)): sha(path) for path in sorted(paths)}


def verify_inputs(kernel, initrd, source_hashes):
    receipt_path = REPO / "build/shizukudos/kernels-build-result.json"
    sample_path = initrd.parent / "build-result.json"
    if not receipt_path.is_file() or not sample_path.is_file():
        raise RuntimeError("kernel/sample build receipts are missing; build the components from stable source first")
    receipt, samples = json.loads(receipt_path.read_text()), json.loads(sample_path.read_text())
    if receipt.get("source_files_sha256") != source_hashes:
        raise RuntimeError("source has changed since the kernel build; rebuild the kernels before assembling a publishable ISO")
    if receipt["kernels"]["kernel64-standalone"]["sha256"] != sha(kernel):
        raise RuntimeError("standalone kernel hash differs from its build receipt")
    expected_sample_sources = samples.get("source_files_sha256", {})
    if not expected_sample_sources or any(
            source_hashes.get(name) != digest for name, digest in samples["source_files_sha256"].items()):
        raise RuntimeError("application sources differ from their build receipt; rebuild tools/build_apps.py")
    if samples["archive"]["sha256"] != sha(initrd):
        raise RuntimeError("native archive hash differs from its build receipt")
    for name, record in samples.get("apps", {}).items():
        path = Path(record.get("file", str(initrd.parent / name)))
        if not path.is_file() or record["sha256"] != sha(path):
            raise RuntimeError("native application hash differs from its build receipt: " + name)


def source_archive(output):
    """Ship corresponding source, licenses and build scripts without build products."""
    hashes = {}
    with output.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as archive:
            for name in source_manifest():
                path = REPO / name
                rel = path.relative_to(REPO)
                info = archive.gettarinfo(str(path), arcname="ShizukuDOS/" + str(rel))
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mtime = 0
                content = path.read_bytes()
                info.size = len(content)
                archive.addfile(info, io.BytesIO(content))
                hashes[str(rel)] = hashlib.sha256(content).hexdigest()
    return hashes


def bios_image(work):
    mod = module(REPO / "shizukudos/build_image.py", "dos_fat12")
    boot, stage = work / "boot.bin", work / "stage2.bin"
    run(["nasm", "-f", "bin", "-o", boot, REPO / "shizukudos/boot.asm"])
    run(["nasm", "-f", "bin", "-I", str(REPO / "shizukudos") + "/", "-o", stage, REPO / "shizukudos/stage2.asm"])
    files = {
        "HELLO.TXT": b"Hello from the independent ShizukuDOS ISO.\r\n",
        "README.TXT": (b"ShizukuDOS 10 DOS-only optical-media track.\r\n"
                       b"BIOS: original 16-bit shell and COM16 samples.\r\n"
                       b"UEFI64: ShizukuGUI, actual MZ16/PE32/PE64 apps, kurazy.\r\n"
                       b"Sources and licenses are in the ISO SOURCE directory.\r\n"),
    }
    for source, name in (("demo_com.asm", "DEMO.COM"), ("ret_com.asm", "RET.COM"), ("std_com.asm", "STD.COM")):
        binary = work / name
        run(["nasm", "-f", "bin", "-o", binary, REPO / "shizukudos/tests" / source])
        files[name] = binary.read_bytes()
    image = work / "dos16.img"
    image.write_bytes(mod.make_image(boot.read_bytes(), stage.read_bytes(), files))
    return image


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--skip-build", action="store_true", help="use existing kernel and DOS64 archive")
    p.add_argument("--out", type=Path, default=DEFAULT_ISO)
    p.add_argument("--kernel", type=Path, default=REPO / "build/shizukudos/kernel64s/KERNEL64S.BIN")
    p.add_argument("--initrd", type=Path, default=REPO / "build/native/DOS64.IMG")
    p.add_argument("--cmdline", default="shz.dos64=1 shz.gui=1")
    p.add_argument("--allow-unverified-inputs", action="store_true", help="exploratory image only: permit artifacts without matching source/build receipts")
    args = p.parse_args()
    for name in ("nasm", "x86_64-w64-mingw32-gcc", "xorriso", "mformat", "mmd", "mcopy"):
        if not shutil.which(name):
            p.error("required tool missing: " + name)
    if not args.skip_build:
        run([sys.executable, REPO / "shizukudos/kbuild.py", "--standalone-only"])
        run([sys.executable, REPO / "samples/dos64/build.py"])
        run([sys.executable, REPO / "tools/build_mode_apps.py"])
        run([sys.executable, REPO / "tools/build_apps.py"])
    for path in (args.kernel, args.initrd):
        if not path.is_file():
            p.error("required build product missing: " + str(path))
    initial_sources = source_manifest()
    if not args.allow_unverified_inputs:
        verify_inputs(args.kernel, args.initrd, initial_sources)
    args.out = args.out.resolve()
    work = REPO / "build/iso"
    # This directory is owned by this builder. Other component outputs stay intact.
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    tree = work / "tree"
    (tree / "BOOT").mkdir(parents=True)
    (tree / "SOURCE").mkdir()
    (tree / "SAMPLES").mkdir()
    shutil.copy2(bios_image(work), tree / "BOOT/DOS16.IMG")
    efi = module(REPO / "shizukudos/standalone_uefi/build.py", "dos_uefi").build(
        args.kernel, args.initrd, REPO / "build/standalone-uefi/BOOTX64.EFI", args.cmdline)
    (tree / "EFI/BOOT").mkdir(parents=True)
    shutil.copy2(efi, tree / "EFI/BOOT/BOOTX64.EFI")
    esp = tree / "BOOT/EFI.IMG"
    esp_mib = max(16, (efi.stat().st_size + (8 << 20) + (1 << 20) - 1) >> 20)
    with esp.open("wb") as out:
        out.truncate(esp_mib << 20)
    run(["mformat", "-i", esp, "-v", "SHIZUKUDOS", "::"])
    run(["mmd", "-i", esp, "::/EFI", "::/EFI/BOOT"])
    run(["mcopy", "-i", esp, efi, "::/EFI/BOOT/BOOTX64.EFI"])
    for directory in (args.initrd.parent, REPO / "build/modes", REPO / "build/dos64"):
        for path in sorted(directory.glob("*")):
            if path.is_file() and (path.suffix.upper() in (".EXE", ".SD64") or path == args.initrd):
                shutil.copy2(path, tree / "SAMPLES" / path.name)
    shutil.copy2(REPO / "LICENSE", tree / "SOURCE/LICENSE")
    shutil.copy2(REPO / "THIRD_PARTY.md", tree / "SOURCE/THIRD_PARTY.md")
    source_hashes = source_archive(tree / "SOURCE/SHIZUKUDOS-SOURCE.tar.gz")
    if source_hashes != initial_sources:
        raise RuntimeError("source changed while assembling the ISO; build again from stable source")
    (tree / "README.TXT").write_text(
        "ShizukuDOS 10 — independent DOS-only distribution\n\n"
        "BIOS optical boot starts the original real-mode DOS shell. Try HELP, DIR,\n"
        "TYPE HELLO.TXT and EXEC DEMO.COM. UEFI x86-64 optical boot starts the\n"
        "native standalone Kernel64, checks all three execution modes and opens\n"
        "ShizukuGUI on the 32-bit GOP framebuffer. Real Mode MZ EXEs have no\n"
        "suffix; protected PE32 utilities end in 32; PE32+ utilities end in 64.\n"
        "F1 browser, F2 video, F3 music, F4 thread tree, F5 utilities, F10 quit.\n"
        "The browser supports packaged local HTML; media formats and the scoped\n"
        "DOS API are documented in the complete source and kurazy specification.\n"
        "It does not require VMX, Windows media, CSMWrap or a Windows runtime.\n"
        "UEFI Secure Boot signing is not supplied. See docs/STATUS.md for scope.\n\n"
        "SOURCE includes corresponding source, GPLv2 license and license manifest.\n"
        "The Windows 98 Shizuku Modern Edition ISO is a separate release track.\n", encoding="utf-8")
    manifest = {"product": "ShizukuDOS", "version": "10.0.0", "track": "dos-only",
                "profiles": {"bios": "original real-mode DOS16 shell, COM16", "uefi-x86_64": "ShizukuGUI GOP32, native MZ16/PE32/PE32+, kurazy, no VMX"},
                "cmdline": args.cmdline, "kernel_sha256": sha(args.kernel), "initrd_sha256": sha(args.initrd),
                "inputs_verified": not args.allow_unverified_inputs,
                "source_hashes": source_hashes,
                "files": {str(path.relative_to(tree)): {"sha256": sha(path), "bytes": path.stat().st_size}
                          for path in sorted(tree.rglob("*")) if path.is_file()}}
    (tree / "MANIFEST.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    run(["xorriso", "-as", "mkisofs", "-iso-level", "3", "-R", "-J", "-V", "SHIZUKUDOS10",
         "-b", "BOOT/DOS16.IMG", "-c", "BOOT/BOOT.CAT", "-eltorito-alt-boot", "-e", "BOOT/EFI.IMG",
         "-no-emul-boot", "-o", args.out, tree])
    if source_manifest() != initial_sources:
        raise RuntimeError("source changed before ISO assembly finished; do not distribute this exploratory build")
    result = {"status": "BUILT", "iso": str(args.out), "bytes": args.out.stat().st_size,
              "sha256": sha(args.out), "manifest": manifest}
    (work / "build-result.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    args.out.with_suffix(args.out.suffix + ".sha256").write_text(sha(args.out) + "  " + args.out.name + "\n")
    print(f"Built {args.out} ({args.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
