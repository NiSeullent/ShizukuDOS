#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify genuine native PE32+ images and exercise the actual shared C PE parser."""
import ctypes
import importlib.util
import json
import struct
import subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/native"
spec = importlib.util.spec_from_file_location("build_apps", ROOT / "tools/build_apps.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

def main():
    receipt = json.loads((OUT / "build-result.json").read_text())
    for name, app in receipt["apps"].items():
        path = Path(app["file"])
        assert mod.inspect_pe(path)["dll_dependencies"] == []
        assert mod.sha(path) == app["sha256"], name
    lib = OUT / "pe-parse-host-test.so"
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", str(ROOT / "shizukudos/win64/pe_parse.c"), "-o", str(lib)], check=True)
    parser = ctypes.CDLL(str(lib))
    parser.pe_parse.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p]
    parser.pe_parse.restype = ctypes.c_int
    def parse(data):
        buf = ctypes.create_string_buffer(bytes(data))
        output = ctypes.create_string_buffer(4096)
        return parser.pe_parse(buf, len(data), output)
    b = bytearray((OUT / "HELLO64.EXE").read_bytes())
    nt = struct.unpack_from("<I", b, 0x3c)[0]
    opt = nt+24
    assert parse(b) == 0
    bad = bytearray(b); struct.pack_into("<I", bad, 0x3c, 0xfffffff0)
    cases = {"truncated": b[:64], "overflowing NT header": bad}
    bad = bytearray(b); struct.pack_into("<H", bad, nt+4, 0x14c); cases["wrong CPU"] = bad
    bad = bytearray(b); struct.pack_into("<H", bad, opt, 0x10b); cases["PE32 optional header"] = bad
    bad = bytearray(b); struct.pack_into("<I", bad, opt+16, struct.unpack_from("<I", bad, opt+56)[0]); cases["entry outside image"] = bad
    bad = bytearray(b); struct.pack_into("<I", bad, opt+56, 0xffffffff); cases["oversized image"] = bad
    results = {}
    for label, data in cases.items():
        rc = parse(data)
        assert rc < 0, label
        results[label] = rc
    result = {"verified_import_free_PE32plus": len(receipt["apps"]), "shared_C_parser_negative_cases": results, "runtime_checks": "CHECK64, THREAD64 and native-loader mutation checks must also pass in the guest"}
    (OUT / "host-test-result.json").write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))
if __name__ == "__main__":
    main()
