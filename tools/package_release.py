#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package the exact verified DOS-only ISO, native apps, and corresponding source."""
import argparse
import gzip
import hashlib
import io
import json
import re
import shutil
import subprocess
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", default="10.0.1")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "releases")
    ap.add_argument("--candidate", action="store_true", help="package an unpublished QEMU candidate before VirtualBox validation")
    ap.add_argument("--virtualbox-evidence", type=Path, default=ROOT / "build" / "virtualbox-tests")
    args = ap.parse_args()
    if not re.fullmatch(r"[0-9][0-9A-Za-z.-]*", args.version):
        raise SystemExit("invalid release version")
    if git("status", "--porcelain").strip():
        raise SystemExit("commit the standalone source before packaging the release")
    iso = ROOT / "build" / "shizukudos-10-dos-only.iso"
    result_path = ROOT / "build" / "iso-tests" / "result.json"
    result = json.loads(result_path.read_text())
    profiles = result.get("profiles", {})
    required_profiles = {"bios", "uefi", "uefi-usb", "uefi-disk", "uefi-sata", "uefi-ia32", "uefi-no-uart"}
    profile_checks = [c for name in required_profiles for c in profiles.get(name, {}).get("checks", [])]
    if (result.get("status") != "PASS" or result.get("iso_sha256") != digest(iso)
            or result.get("inputs_verified") is not True
            or set(profiles) != required_profiles
            or any(not profiles[name].get("checks") for name in required_profiles)
            or any(profiles[name].get("audio", {}).get("signal_verified") is not True for name in required_profiles if name != "bios")
            or not all(c.get("status") == "PASS" for c in profile_checks)):
        raise SystemExit("release refused: ISO acceptance did not pass for these exact ISO bytes")
    virtualbox = {"status": "NOT_RUN"}
    if not args.candidate:
        vb_path = args.virtualbox_evidence.resolve() / "result.json"
        virtualbox = json.loads(vb_path.read_text())
        vb_profiles = virtualbox.get("profiles", {})
        if (virtualbox.get("schema") != "shizukudos.virtualbox.v1"
                or virtualbox.get("status") != "PASS"
                or virtualbox.get("iso_sha256") != digest(iso)
                or virtualbox.get("host", {}).get("status") != "AVAILABLE"
                or not re.match(r"7\.", virtualbox.get("host", {}).get("version", ""))
                or set(vb_profiles) != {"efi32", "efi64"}
                or any(p.get("status") != "PASS" or not p.get("checks")
                       or any(c.get("status") != "PASS" for c in p["checks"])
                       for p in vb_profiles.values())):
            raise SystemExit("release refused: actual VirtualBox EFI32/EFI64 acceptance did not pass for these exact ISO bytes")
    commit = git("rev-parse", "HEAD").decode().strip()
    epoch = int(git("show", "-s", "--format=%ct", "HEAD"))
    tracked = [p.decode() for p in git("ls-files", "-z").split(b"\0") if p]
    source_hashes = {name: digest(ROOT / name) for name in tracked}
    if source_hashes != result.get("source_hashes"):
        raise SystemExit("release refused: current source differs from the source on the tested ISO; rebuild and retest")
    apps = ROOT / "build" / "native"
    samples = {}
    for name, record in result.get("payload_hashes", {}).items():
        if not name.startswith("SAMPLES/"):
            continue
        filename = Path(name).name
        candidates = [ROOT / "build" / directory / filename for directory in ("native", "modes", "dos64")]
        path = next((p for p in candidates if p.is_file() and digest(p) == record["sha256"]), None)
        if path is None:
            raise SystemExit("release refused: application artifact differs from the tested ISO: " + name)
        samples[filename] = path
    library = apps / "libkurazy64.a"
    if digest(library) != result.get("initrd_files", {}).get("\\SDK\\LIBKURAZY64.A"):
        raise SystemExit("release refused: static SDK differs from the SDK on the tested ISO")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stem = f"shizukudos-{args.version}"
    dist_iso = out / f"{stem}-dos-only.iso"
    shutil.copyfile(iso, dist_iso)
    source = out / f"{stem}-source.tar.gz"
    archive = git("archive", "--format=tar", f"--prefix={stem}/", "HEAD")
    source.write_bytes(gzip.compress(archive, mtime=0))
    sample_zip = out / f"{stem}-native-apps-and-sdk.zip"
    with zipfile.ZipFile(sample_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in sorted((ROOT / "samples").rglob("*")) + sorted((ROOT / "sdk" / "kurazy").rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                zf.write(path, str(path.relative_to(ROOT)))
        for name, path in sorted(samples.items()):
            zf.write(path, f"bin/{name}")
        zf.write(library, "lib/" + library.name)
        for name in ("shizukudos/win64/pe_parse.c", "shizukudos/win64/pe_parse.h", "shizukudos/kernel64/cpu_modes_formats.h",
                     "shizukudos/kernel64/kurazy_media.c", "shizukudos/kernel64/kurazy_media.h"):
            zf.write(ROOT / name, name)
        for path in (apps / "build-result.json", ROOT / "tools/build_apps.py", ROOT / "tools/build_mode_apps.py"):
            zf.write(path, str(path.relative_to(ROOT)))
        zf.write(ROOT / "LICENSE", "LICENSE")
        zf.write(ROOT / "SOURCE_ORIGIN.json", "SOURCE_ORIGIN.json")
    evidence = out / f"{stem}-evidence.tar.gz"
    # Only this release's isolated tests are included, never another project's build tree.
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w") as tf:
        evidence_paths = [ROOT / "build" / "iso-tests", ROOT / "build" / "native" / "validation", ROOT / "build" / "native" / "host-test-result.json",
                          ROOT / "build" / "dos64" / "validation", ROOT / "build" / "modes" / "validation"]
        if not args.candidate:
            evidence_paths.append(args.virtualbox_evidence.resolve())
        for base in evidence_paths:
            if not base.exists():
                continue
            for path in [base] if base.is_file() else sorted(base.rglob("*")):
                if not path.is_file() or path.suffix not in (".json", ".jsonl", ".log", ".txt", ".png", ".wav"):
                    continue
                rel = (Path("virtualbox-tests") / path.relative_to(args.virtualbox_evidence.resolve())
                       if not args.candidate and path.is_relative_to(args.virtualbox_evidence.resolve())
                       else path.relative_to(ROOT / "build"))
                info = tf.gettarinfo(str(path), str(rel))
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mtime = epoch
                with path.open("rb") as fh:
                    tf.addfile(info, fh)
    evidence.write_bytes(gzip.compress(stream.getvalue(), mtime=0))
    assets = [dist_iso, source, sample_zip, evidence]
    manifest = out / f"{stem}-manifest.json"
    manifest.write_text(json.dumps({"schema": "shizukudos.release.v1", "version": args.version,
                                   "repository": "https://github.com/NiSeullent/ShizukuDOS",
                                   "source_commit": commit,
                                   "release_state": "candidate" if args.candidate else "verified",
                                   "upstream": json.loads((ROOT / "SOURCE_ORIGIN.json").read_text())["upstream_commit"],
                                   "iso_acceptance": {"status": result["status"], "sha256": digest(iso)},
                                   "virtualbox_acceptance": {"status": virtualbox["status"],
                                       "sha256": virtualbox.get("iso_sha256"),
                                       "version": virtualbox.get("host", {}).get("version"),
                                       "profiles": {k: v["status"] for k, v in virtualbox.get("profiles", {}).items()}},
                                   "source_files_sha256": source_hashes,
                                   "assets": {p.name: {"bytes": p.stat().st_size, "sha256": digest(p)} for p in assets}},
                                  indent=2) + "\n")
    assets.append(manifest)
    hashes = out / "SHA256SUMS"
    hashes.write_text("".join(f"{digest(p)}  {p.name}\n" for p in assets))
    print(json.dumps({"source_commit": commit, "assets": [str(p) for p in [*assets, hashes]]}, indent=2))


if __name__ == "__main__":
    main()
