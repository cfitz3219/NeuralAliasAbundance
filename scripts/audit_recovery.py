#!/usr/bin/env python3
"""Verify recovered evidence and inventory build gaps; does not build or run tests."""
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def normalized(data):
    return data.replace(b"\r\n", b"\n").strip()


def build_inventory():
    cmake = (ROOT / "CMakeLists.txt").read_text()
    paths = set(re.findall(
        r"(?<![\w/])(?:src|tests|third_party|tools)/[\w./${}-]+\.(?:cpp|c|hpp|h|def)",
        cmake,
    ))
    for name in ("temporal_jitter", "frame_history", "upload_cache"):
        paths.add(f"tests/{name}_test.cpp")
    return {
        "baseline_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "missing_build_sources": sorted(p for p in paths if "$" not in p and not (ROOT / p).is_file()),
        "recovered_cpp_not_in_cmake": sorted(
            p.relative_to(ROOT).as_posix() for p in (ROOT / "src").rglob("*.cpp")
            if p.relative_to(ROOT).as_posix() not in cmake
        ),
        "limitations": "Static path inventory, including the three known foreach tests; not a general CMake parser or build validation.",
    }


def main():
    manifest = json.loads((ROOT / "docs/recovery/manifest.json").read_text())
    for item in manifest["uploads"]:
        data = (ROOT / item["path"]).read_bytes()
        if len(data) != item["bytes"] or sha(data) != item["sha256"]:
            raise ValueError(f"Upload integrity mismatch: {item['path']}")
    bundle = (ROOT / "docs/recovery/originals/neural_alias_codebase.txt").read_bytes()
    changed = []
    for item in manifest["extracted_files"]:
        body = bundle[item["bundle_start"]:item["bundle_end"]]
        if len(body) != item["bytes"] or sha(body) != item["sha256"]:
            raise ValueError(f"Bundle span mismatch: {item['path']}")
        baseline = subprocess.check_output([
            "git", "show", f"{manifest['claude_baseline_commit']}:{item['path']}"
        ], cwd=ROOT)
        if sha(baseline) != item["claude_recovery_sha256"] or normalized(body) != normalized(baseline):
            raise ValueError(f"Recovered baseline mismatch: {item['path']}")
        current = ROOT / item["path"]
        if not current.is_file() or sha(current.read_bytes()) != item["claude_recovery_sha256"]:
            changed.append(item["path"])
    print(f"PASS: {len(manifest['uploads'])} uploads and {len(manifest['extracted_files'])} exported source bodies verified against the recorded Git baseline.")
    print(f"Worktree files changed since that baseline: {len(changed)} (later development is permitted).")
    for path in changed:
        print(f"  {path}")
    print(json.dumps(build_inventory(), indent=2))
    print("No application build or runtime tests were executed by this audit.")


if __name__ == "__main__":
    main()
