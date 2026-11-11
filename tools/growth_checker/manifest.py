"""Evidence manifests: what a piece of evidence is and where it came from.

The producer build is read from the evidence itself, never from a label a caller
supplies. A saved folder with several status logs yields one manifest per log:
no run is dropped and none is picked for being the largest.
"""
import hashlib
import json
import os
import re

import statuslog
from version import SCHEMA, VERSION

STATUS_NAME = re.compile(r"^status-(.+)\.(?:log|txt)$")
FOLDER_NAME = re.compile(r"^NA-(?P<profile>.+?)-results-(?P<stamp>\d{8}-\d{6})")
SUMMARY_BUILD = re.compile(r"^Build:\s*([0-9a-fA-F]{7,40})\s*$", re.M)
# Strongest first. "results_summary" is the text file the saving script wrote, so
# it says which package saved the folder, not which program wrote the log.
BUILD_SOURCES = ("status_header", "startup_header", "run_manifest", "results_summary", "unknown")
EVIDENCE_TYPES = ("live_run", "replay", "fixture_test", "synthetic")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def _file(path, role):
    return {"name": os.path.basename(path), "role": role, "bytes": os.path.getsize(path), "sha256": sha256(path)}


def status_logs(path):
    """The status logs of a saved folder (or the one file given), sorted by name."""
    if os.path.isfile(path):
        return [path]
    names = sorted(n for n in os.listdir(path) if STATUS_NAME.match(n))
    return [os.path.join(path, n) for n in names]


def producer_build(parsed, status_path):
    """-> (build or None, source, notes). Looks only at the evidence."""
    notes = []
    distinct = sorted(set(parsed["builds"]))
    if len(distinct) > 1:
        return None, "unknown", ["the status log carries build lines for different builds: " + ", ".join(distinct)]
    if distinct:
        return distinct[0], "status_header", notes
    folder = os.path.dirname(status_path)
    key = STATUS_NAME.match(os.path.basename(status_path))
    if key:
        for ext in ("log", "txt"):
            startup = os.path.join(folder, f"startup-{key[1]}.{ext}")
            if os.path.isfile(startup):
                first = read_text(startup).splitlines()[:1]
                m = statuslog.BUILD.search(first[0]) if first else None
                if m:
                    return m[1].lower(), "startup_header", notes
    run_manifest = os.path.join(folder, "run-manifest.json")
    if os.path.isfile(run_manifest):
        try:
            build = json.loads(read_text(run_manifest)).get("build")
            if isinstance(build, str) and re.fullmatch(r"[0-9a-fA-F]{7,40}", build):
                return build.lower(), "run_manifest", notes
        except ValueError:
            notes.append("run-manifest.json could not be read")
    for name in ("summary.txt", "run-summary.txt"):
        summary = os.path.join(folder, name)
        if os.path.isfile(summary):
            m = SUMMARY_BUILD.search(read_text(summary))
            if m:
                notes.append(f"build taken from {name}, which the saving script wrote; the log itself carries no build line")
                return m[1].lower(), "results_summary", notes
    return None, "unknown", ["no build line in the evidence"]


def build_matches(build, expected):
    """True when both name the same commit (either may be an abbreviation)."""
    if not build or not expected:
        return False
    a, b = build.lower(), expected.lower()
    return a.startswith(b) or b.startswith(a)


def make(status_path, parsed, evidence_type="live_run", case=None, case_source=None):
    """The manifest of one status log. `parsed` is statuslog.parse() of it."""
    if evidence_type not in EVIDENCE_TYPES:
        raise ValueError(f"evidence type must be one of {EVIDENCE_TYPES}")
    folder = os.path.dirname(os.path.abspath(status_path))
    build, source, notes = producer_build(parsed, status_path)
    files = [_file(status_path, "status")]
    key = STATUS_NAME.match(os.path.basename(status_path))
    session = key[1] if key else None
    if key:
        for role in ("startup", "observe"):
            for ext in ("log", "txt"):
                other = os.path.join(folder, f"{role}-{key[1]}.{ext}")
                if os.path.isfile(other):
                    files.append(_file(other, role))
    for name in ("run-manifest.json", "summary.txt", "run-summary.txt"):
        other = os.path.join(folder, name)
        if os.path.isfile(other) and os.path.abspath(other) != os.path.abspath(status_path):
            files.append(_file(other, "saved-with-run"))
    fm = FOLDER_NAME.match(os.path.basename(folder))
    samples = parsed["samples"]
    digest = files[0]["sha256"]
    return {
        "schema": f"growth.manifest/{SCHEMA}",
        # Stable for the same evidence however often it is scored again.
        "evidence_id": f"{(build or 'nobuild')[:8]}-{digest[:12]}",
        "evidence_type": evidence_type,
        "producer": {"build": build, "build_source": source},
        "session": {
            "id": session,
            "first_sample": statuslog.clock(samples[0]["t"]) if samples else None,
            "last_sample": statuslog.clock(samples[-1]["t"]) if samples else None,
            "saved_folder": os.path.basename(folder),
            "saved_at": fm["stamp"] if fm else None,
            "profile_hint": fm["profile"] if fm else None,
        },
        "case": case,
        "case_source": case_source if case else None,
        "files": files,
        "scorer": {"tool": "growth_checker", "version": VERSION, "schema": SCHEMA},
        "notes": notes,
    }
