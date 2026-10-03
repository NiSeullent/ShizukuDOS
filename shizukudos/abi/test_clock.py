#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bounded portable Core clock tests; no kernel download, ISO build or guest."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
ABI = ROOT / "shizukudos/abi"
DOMAIN = ROOT / "shizukudos/supervisor/src/domain.c"
BUILD = ROOT / "shizukudos/supervisor/build.py"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_clock(source):
    """Extract literal production bodies; missing/ambiguous seams fail closed."""
    functions = re.findall(r"static int32_t clock_sample\(void \*opaque, uint64_t \*value\)\n"
                           r"\{[^{}]*\n\}", source)
    cases = re.findall(r"    case SHZ_HC_CLOCK_SPLIT:\n.*?(?=    case )", source, re.S)
    if len(functions) != 1 or len(cases) != 1:
        raise ValueError("expected exactly one actual clock producer and HC15 case")
    return (functions[0] + "\nstatic int32_t core_clock_dispatch(uint64_t *r)\n{\n"
            "    int32_t status = SHZ_OK;\n    switch (r[GPR_RAX]) {\n" + cases[0] +
            "    default: status = SHZ_E_UNSUPPORTED; break;\n    }\n"
            "    return status;\n}\n")


def payload_flags():
    """Read the actual component flags without executing the build entrypoint."""
    tree = ast.parse(BUILD.read_text())
    values = [ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
              and any(isinstance(t, ast.Name) and t.id == "CFLAGS" for t in n.targets)]
    if len(values) != 1 or not all(isinstance(v, str) for v in values[0]):
        raise ValueError("expected one literal Supervisor CFLAGS list")
    return values[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--gcc-sanitizer-libdir", type=Path,
                        help="Optional isolated GCC sanitizer runtimes under this clone's build/")
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / "build") or out == ROOT / "build" or out.exists():
        parser.error("--out must be a fresh directory under this clone's ignored build/")
    gcc_runtime_flags, runtime_inputs = [], {}
    if args.gcc_sanitizer_libdir:
        libdir = args.gcc_sanitizer_libdir.resolve()
        if not libdir.is_relative_to(ROOT / "build") or not libdir.is_dir():
            parser.error("isolated sanitizer directory must be under this clone's build/")
        for name in ("libasan.so", "libubsan.so"):
            library = (libdir / name).resolve(strict=True)
            if not library.is_relative_to(libdir) or not library.is_file():
                parser.error("sanitizer library must resolve to an actual local regular file")
            runtime_inputs[str(library.relative_to(ROOT))] = digest(library)
        gcc_runtime_flags = ["-L", libdir, f"-Wl,-rpath,{libdir}"]
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    new = ["shizukudos/abi/shz_clock.h", "shizukudos/abi/test_clock.c", "shizukudos/abi/test_clock.py"]
    paths = sorted({n for n in tracked + new if n and (ROOT / n).is_file()})
    before = {n: digest(ROOT / n) for n in paths}
    out.mkdir(parents=True)
    commands, tools = [], {}
    status = "FAIL"

    def run(command):
        cmd = [str(x) for x in command]
        started = time.monotonic()
        log = out / f"{len(commands):02d}.log"
        try:
            result = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=60)
        except subprocess.TimeoutExpired as error:
            log.write_bytes((error.stdout or b"") + (error.stderr or b""))
            commands.append({"command": cmd, "exit_code": None, "timeout_seconds": 60,
                             "elapsed_seconds": time.monotonic() - started,
                             "log": str(log.relative_to(ROOT)), "log_sha256": digest(log)})
            raise
        log.write_bytes(result.stdout + result.stderr)
        commands.append({"command": cmd, "exit_code": result.returncode,
                         "elapsed_seconds": time.monotonic() - started,
                         "log": str(log.relative_to(ROOT)), "log_sha256": digest(log)})
        if result.returncode:
            raise RuntimeError(f"command {len(commands)-1} failed; see {log}")
        print(result.stdout.decode().strip() or f"PASS command {len(commands)-1}", flush=True)

    try:
        literal = extract_clock(DOMAIN.read_text())
        (out / "domain_clock.inc").write_text(literal)
        # The seam extractor must refuse omitted or ambiguous production bodies.
        for bad in (DOMAIN.read_text().replace("case SHZ_HC_CLOCK_SPLIT:", "case 999:"),
                    DOMAIN.read_text() + "\n" + DOMAIN.read_text()):
            try:
                extract_clock(bad)
            except ValueError:
                pass
            else:
                raise RuntimeError("ambiguous/missing production seam accepted")
        flags = payload_flags()
        for cc in ("gcc", "clang"):
            tools[cc] = subprocess.check_output([cc, "--version"], text=True, timeout=10).splitlines()[0]
            binary = out / f"clock-{cc}"
            run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
                 "-Wconversion", "-Wshadow", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-fno-pie", "-no-pie", *(gcc_runtime_flags if cc == "gcc" else []),
                 "-I", ABI, "-I", out, ABI / "test_clock.c", "-o", binary])
            run([binary])
            component_flags = [v for v in flags if cc != "clang" or v != "-fno-tree-loop-distribute-patterns"]
            run([cc, *component_flags, "-I", DOMAIN.parent, "-I", ABI, "-c", DOMAIN,
                 "-o", out / f"domain-{cc}.o"])
        if {n: digest(ROOT / n) for n in paths} != before:
            raise RuntimeError("source changed during component checks")
        if any(digest(ROOT / n) != sha for n, sha in runtime_inputs.items()):
            raise RuntimeError("isolated sanitizer runtime changed during component checks")
        status = "PASS_CORE_CLOCK_COMPONENTS_ONLY"
    finally:
        after = {n: digest(ROOT / n) for n in paths}
        artifacts = {str(p.relative_to(ROOT)): {"bytes": p.stat().st_size, "sha256": digest(p)}
                     for p in out.iterdir() if p.is_file()}
        receipt = {"status": status, "source_before": before, "source_after": after,
                   "source_before_after_match": before == after, "commands": commands,
                   "tools": tools, "artifacts": artifacts,
                   "isolated_runtime_inputs": runtime_inputs,
                   "scope": "Actual shared clock functions and literal Supervisor producer/HC15 case; modeled TSC/registers; actual full domain translation unit compile.",
                   "sanitizers": {"gcc": True, "clang": True},
                   "whole_hypercall_TSC_read_count_verified": False,
                   "VM_executed": False, "ISO_rebuilt": False,
                   "Windows98_boot_verified": False, "native_apps_verified": False,
                   "clock_hardware_resolution_verified": False}
        (out / "result.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"{status}: {out / 'result.json'}")


if __name__ == "__main__":
    main()
