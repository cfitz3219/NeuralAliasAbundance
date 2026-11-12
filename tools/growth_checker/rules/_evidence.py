"""Small record readers shared by the judgment rules."""
import math
import re

LIVE = "live_run"
STRONG_BUILD_SOURCES = {"status_header", "startup_header", "run_manifest"}


def measurement(result, name):
    return ((result or {}).get("measurements") or {}).get(name) or {}


def number(result, name):
    entry = measurement(result, name)
    value = entry.get("value")
    if entry.get("state") == "measured" and isinstance(value, (int, float)):
        if not isinstance(value, bool) and math.isfinite(value):
            return value
    return None


def manifest(result):
    return (result or {}).get("manifest") or {}


def build(result):
    return (manifest(result).get("producer") or {}).get("build")


def same_build(a, b):
    if not all(isinstance(s, str) and re.fullmatch(r"[0-9a-fA-F]{7,40}", s) for s in (a, b)):
        return False
    return a.lower().startswith(b.lower()) or b.lower().startswith(a.lower())


def status_hash(result):
    for entry in manifest(result).get("files", []):
        if entry.get("role") == "status":
            return entry.get("sha256")
    return None


def same_evidence(a, b):
    first, second = manifest(a).get("evidence_id"), manifest(b).get("evidence_id")
    left, right = status_hash(a), status_hash(b)
    return bool(first and first == second or left and left == right)


def observed(result, name):
    entry = ((result or {}).get("observed") or {}).get(name)
    if isinstance(entry, dict) and "last" in entry:
        return entry["last"]
    return entry


def build_source(result):
    return (manifest(result).get("producer") or {}).get("build_source")


def intervals(result):
    return sum(max(0, session.get("samples", 0) - 1) for session in result.get("sessions", []))


def tolerance_for(name, baseline, candidate):
    if name in {"fps_play_median", "fps_median_while_delivering", "fps_play_lowest", "fps_lowest_while_delivering"}:
        return {"absolute": 1.0, "relative_percent": 3.0}
    if name == "frame_ms_play_median":
        return {"absolute": 0.25, "relative_percent": 3.0}
    if name in {"scene_share_of_presented", "delivered_share_of_scene",
                "delivered_share_of_presented", "not_delivering_share_of_play"}:
        return {"absolute": 1.0, "relative_percent": 0.0}
    if name.startswith("longest_gap_") and name != "longest_gap_started":
        periods = [number(r, "sample_period_seconds") or 0 for r in (baseline, candidate)
                   if measurement(r, name).get("basis") == "sampled"]
        return {"absolute": max(periods, default=0), "relative_percent": 0.0}
    return {"absolute": 0.0, "relative_percent": 0.0}


def slack(value, tolerance):
    return max(tolerance.get("absolute", 0), abs(value) * tolerance.get("relative_percent", 0) / 100)
