#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Plan or apply reviewed, exact BIOS-DOS source updates. No fetch/build/guest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile

SCHEMA = "shizukudos.dos-source-sync.v1"
POLICY = "docs/DOS_SOURCE_SYNC.json"
ALLOWED_PATHS = frozenset(("shizukudos/boot.asm", "shizukudos/stage2.asm", "shizukudos/cpu_detect.inc"))
MAX_BLOB = 256 * 1024
UPSTREAM = "https://github.com/NiSeullent/Win98-Modern"

class SyncRefused(ValueError):
    pass

def sha(data):
    return hashlib.sha256(data).hexdigest()

def git(repo, *args):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GIT_OPTIONAL_LOCKS="0")
    for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        env.pop(key, None)
    result = subprocess.run(["git", "-C", str(repo), *args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=15, env=env)
    if result.returncode:
        raise SyncRefused("local Git read failed: " + args[0])
    return result.stdout

def root_path(path):
    path = Path(path).resolve(strict=True)
    actual = Path(git(path, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    if path != actual:
        raise SyncRefused("an exact repository root is required")
    return path

def regular(repo, relative):
    path = repo
    for part in Path(relative).parts:
        path = path / part
        if path.is_symlink():
            raise SyncRefused("symlink in target path: " + relative)
    if not stat.S_ISREG(path.stat().st_mode):
        raise SyncRefused("target is not a regular file: " + relative)
    return path

def bounded_read(path):
    if path.stat().st_size > MAX_BLOB:
        raise SyncRefused("source or metadata size bound exceeded")
    data = path.read_bytes()
    if len(data) > MAX_BLOB:
        raise SyncRefused("source or metadata changed size")
    return data

def blob(repo, commit, path):
    row = git(repo, "ls-tree", "-z", commit, "--", path).split(b"\0")
    if len(row) != 2 or not row[0]:
        raise SyncRefused("missing exact source path: " + path)
    info, name = row[0].split(b"\t", 1)
    mode, kind, oid = info.decode().split()
    if name.decode() != path or mode != "100644" or kind != "blob":
        raise SyncRefused("source must be a non-executable regular Git blob: " + path)
    if int(git(repo, "cat-file", "-s", oid)) > MAX_BLOB:
        raise SyncRefused("source blob size bound exceeded")
    data = git(repo, "cat-file", "blob", oid)
    if len(data) > MAX_BLOB or b"\0" in data:
        raise SyncRefused("binary or oversized source refused: " + path)
    return data

def load_policy(target):
    raw = bounded_read(regular(target, POLICY))
    policy = json.loads(raw)
    if policy.get("schema") != SCHEMA or policy.get("upstream_repository") != UPSTREAM:
        raise SyncRefused("unknown sync policy")
    files = policy.get("files", {})
    if not isinstance(files, dict) or not files or not set(files) <= ALLOWED_PATHS:
        raise SyncRefused("policy path outside the exact reviewed allowlist")
    for value in files.values():
        if not isinstance(value, dict) or not re.fullmatch(r"[0-9a-f]{64}", value.get("baseline_sha256", "")):
            raise SyncRefused("invalid shared baseline")
    origin = bounded_read(regular(target, "SOURCE_ORIGIN.json"))
    if sha(origin) != policy.get("source_origin_sha256"):
        raise SyncRefused("immutable initial source origin changed")
    return policy, raw, origin

def plan_sync(target, source, commit, paths=None, handoff=None):
    target, source = root_path(target), root_path(source)
    if target == source:
        raise SyncRefused("source and target repositories must be separate")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise SyncRefused("a complete immutable source commit is required")
    if git(source, "cat-file", "-t", commit).strip() != b"commit":
        raise SyncRefused("source revision is not a commit")
    upstream = git(source, "config", "--get", "remote.origin.url").decode().strip().removesuffix(".git").rstrip("/")
    if upstream != UPSTREAM:
        raise SyncRefused("source checkout origin does not match the reviewed upstream")
    policy, raw, origin = load_policy(target)
    selected = sorted(policy["files"] if paths is None else paths)
    if not selected or len(selected) != len(set(selected)) or not set(selected) <= set(policy["files"]):
        raise SyncRefused("selected path outside the exact reviewed policy")
    approval = None
    if handoff is not None:
        approval = json.loads(bounded_read(Path(handoff)))
        if (not isinstance(approval, dict) or approval.get("approved") is not True or approval.get("source_commit") != commit
            or set(approval) != {"owner", "approved", "source_commit", "paths"}
            or not isinstance(approval.get("owner"), str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", approval["owner"])
            or not isinstance(approval.get("paths"), list)
            or not all(isinstance(path, str) for path in approval["paths"])
            or not set(selected) <= set(approval["paths"]) <= ALLOWED_PATHS):
            raise SyncRefused("reviewed source-owner handoff does not match this source selection")
    incoming, old, entries = {}, {}, []
    for path in selected:
        incoming[path] = blob(source, commit, path)
        old[path] = bounded_read(regular(target, path))
        base = policy["files"][path]["baseline_sha256"]
        src, dst = sha(incoming[path]), sha(old[path])
        if src == dst:
            status = "unchanged" if src == base else "converged"
        elif src == base:
            status = "target_only"
        elif dst == base:
            status = "source_only"
        else:
            status = "conflict"
        entries.append({"path": path, "baseline_sha256": base, "source_sha256": src,
                        "target_sha256": dst, "bytes": len(incoming[path]), "status": status})
    report = {"schema": SCHEMA, "upstream_repository": UPSTREAM, "source_commit": commit,
              "target_commit": git(target, "rev-parse", "HEAD").decode().strip(),
              "source_origin_sha256": sha(origin), "policy_sha256": sha(raw),
              "owner_review": approval, "entries": entries, "applied": False,
              "scope": "exact reviewed public BIOS-DOS text sources; no build or runtime claim"}
    return report, (target, policy, raw, origin, incoming, old)

def _replace(path, data):
    mode = stat.S_IMODE(path.stat().st_mode)
    fd, temp = tempfile.mkstemp(prefix=".dos-sync-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temp, mode)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)

def apply_sync(report, state):
    target, policy, raw, origin, incoming, old = state
    if report["owner_review"] is None:
        raise SyncRefused("reviewed source-owner handoff required before writes")
    if any(e["status"] == "conflict" for e in report["entries"]):
        raise SyncRefused("both sides changed; resolve on a separate reviewed branch")
    if git(target, "status", "--porcelain=v1", "--untracked-files=all"):
        raise SyncRefused("clean target branch required before writes")
    if git(target, "rev-parse", "HEAD").decode().strip() != report["target_commit"]:
        raise SyncRefused("target commit changed after planning")
    if bounded_read(regular(target, POLICY)) != raw or bounded_read(regular(target, "SOURCE_ORIGIN.json")) != origin:
        raise SyncRefused("policy or initial source origin changed after planning")
    for path, before in old.items():
        if bounded_read(regular(target, path)) != before:
            raise SyncRefused("target file changed after planning: " + path)
    changed = []
    new_policy = json.loads(raw)
    for entry in report["entries"]:
        if entry["status"] in ("source_only", "converged"):
            new_policy["files"][entry["path"]]["baseline_sha256"] = entry["source_sha256"]
    new_policy["last_reviewed_sync"] = {
        "source_commit": report["source_commit"], "target_before_commit": report["target_commit"],
        "owner_review": report["owner_review"], "entries": report["entries"]}
    try:
        for entry in report["entries"]:
            if entry["status"] == "source_only":
                path = entry["path"]
                _replace(regular(target, path), incoming[path])
                changed.append(path)
        _replace(regular(target, POLICY), (json.dumps(new_policy, indent=2, sort_keys=True) + "\n").encode())
        if bounded_read(regular(target, "SOURCE_ORIGIN.json")) != origin:
            raise SyncRefused("initial source origin drift during apply")
    except BaseException:
        for path in reversed(changed):
            _replace(regular(target, path), old[path])
        _replace(regular(target, POLICY), raw)
        raise
    report["applied"] = True
    return report

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--path", action="append", dest="paths")
    parser.add_argument("--handoff", type=Path, help="operator-reviewed owner handoff JSON; not an authentication token")
    parser.add_argument("--apply", action="store_true", help="write selected sources and ledger on a clean branch")
    args = parser.parse_args()
    target = Path(__file__).resolve().parents[1]
    try:
        report, state = plan_sync(target, args.source, args.commit, args.paths, args.handoff)
        if args.apply:
            apply_sync(report, state)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 2 if any(e["status"] == "conflict" for e in report["entries"]) else 0
    except (SyncRefused, OSError, json.JSONDecodeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, "source sync refused: " + str(error) + "\n")

if __name__ == "__main__":
    raise SystemExit(main())
