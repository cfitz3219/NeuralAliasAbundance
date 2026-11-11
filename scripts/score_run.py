#!/usr/bin/env python3
"""Replay check, part 1: score a saved run from its status log.

A "game" here is a results folder (status-*.log as written by the results CMD)
plus an expected-result file. The same numbers feed the scorecard:
frames with DLAA, the longest stretch without it, fps while it runs, and WHY
frames were missed. No game is needed; nothing here is game specific.

  python3 scripts/score_run.py <results-folder-or-log> [--expect file.json] [--json]
  python3 scripts/score_run.py --self-test

Saved runs are not committed (COLLABORATION.md rule 5). Expected results for
real runs live in tests/replay/expected/ and name the folder they belong to.
"""
import glob, json, os, re, sys

# Where each provider reports "scene frames seen" and "frames DLAA delivered".
# Add a provider by adding a row; nothing else changes.
PROVIDERS = {
    # "fired" = frames where the provider saw a finished scene (menus and videos do not count).
    "native11": (r"\bnative11 frames=\d+ fired=(\d+)", r"\bnative11_dlaa delivered=(\d+)"),
    "dx12": (r"\bj2_coverage presents=(\d+)", r"\bj2_coverage presents=\d+ covered=(\d+)"),
}
SAMPLE = re.compile(r"^(?:\d+:)?(\d\d):(\d\d):(\d\d) fps=([\d.]+)", re.M)
REASONS = re.compile(r"\bpose\{([^}]*)\}")
SPLIT_REASONS = re.compile(r"\bsplit_rejected\{([^}]*)\}")  # why the split-camera vote gave no camera
START_REASONS = re.compile(r"\bstart_rejected\{([^}]*)\}")  # what the no-inverse rules saw on a missed frame
NOT_REASONS = {"native", "legacy", "generic", "split", "started", "continued", "no_inverse"}  # these are successes


def samples(text):
    marks = list(SAMPLE.finditer(text))
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        h, mi, s = int(m[1]), int(m[2]), int(m[3])
        yield h * 3600 + mi * 60 + s, float(m[4]), text[m.start():end]


def score(text):
    best = None
    for name, (frames_re, delivered_re) in PROVIDERS.items():
        rows = []
        for t, fps, block in samples(text):
            f, d = re.search(frames_re, block), re.search(delivered_re, block)
            if f and d:
                r = REASONS.search(block)
                reasons = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", r[1])) if r else {}
                r = SPLIT_REASONS.search(block)
                if r:
                    reasons.update(("split_" + k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", r[1]))
                r = START_REASONS.search(block)
                if r:
                    reasons.update(("start_" + k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", r[1]))
                rows.append((t, fps, int(f[1]), int(d[1]), reasons))
        if rows and (best is None or rows[-1][2] > best[1][-1][2]):
            best = (name, rows)
    if best is None:
        return {"error": "no provider counters found in the log"}
    name, rows = best
    frames, delivered = rows[-1][2] - rows[0][2], rows[-1][3] - rows[0][3]
    # A gap is a run of samples where scenes were drawn and some got no DLAA. Its
    # length counts only the missed share of each sample, so a gap that starts
    # mid-sample is not rounded up to whole samples.
    gap = longest = span = longest_span = 0.0
    gap_start = longest_start = None
    fps_on = []
    for a, b in zip(rows, rows[1:]):
        dt = (b[0] - a[0]) % 86400
        df, dd = b[2] - a[2], b[3] - a[3]
        missed = max(0, df - dd) / df if df > 0 else 0.0
        if missed > 0.02:
            if gap == 0:
                gap_start = a[0]
            gap += dt * missed
            span += dt
            if gap > longest:
                longest, longest_start, longest_span = gap, gap_start, span
        else:
            gap = span = 0.0
        if df > 0 and dd >= 0.98 * df and b[1] > 0:
            fps_on.append(b[1])
    fps_on.sort()
    first, last = rows[0][4], rows[-1][4]
    why = {k: last[k] - first.get(k, 0) for k in last if k not in NOT_REASONS and last[k] - first.get(k, 0) > 0}
    # Which rule found the camera ("native" counts every camera-relative pose, so it overlaps "split").
    found = {k: last[k] - first.get(k, 0) for k in last if k in NOT_REASONS and last[k] - first.get(k, 0) > 0}
    clock = lambda t: None if t is None else "%02d:%02d:%02d" % (t // 3600, t // 60 % 60, t % 60)
    return {
        "provider": name,
        "samples": len(rows),
        "scene_frames": frames,
        "dlaa_frames": delivered,
        "dlaa_percent": round(100.0 * delivered / frames, 1) if frames else 0.0,
        "longest_gap_seconds": round(longest, 1),  # seconds of frames without DLAA in the worst stretch
        "longest_gap_window_seconds": round(longest_span),  # how long that stretch lasted on the clock
        "longest_gap_started": clock(longest_start),
        "fps_median_with_dlaa": fps_on[len(fps_on) // 2] if fps_on else 0.0,
        "fps_lowest_with_dlaa": fps_on[0] if fps_on else 0.0,
        "camera_found_by": found,
        "missed_because": why,
    }


def check(result, expect):
    """expect: {"min": {...}, "max": {...}}; returns the list of failures, each saying why."""
    fails = []
    if "error" in result:
        return [result["error"]]
    for key, low in expect.get("min", {}).items():
        if result.get(key, 0) < low:
            fails.append(f"{key} = {result.get(key)} is below the expected minimum {low}")
    for key, high in expect.get("max", {}).items():
        if result.get(key, 0) > high:
            fails.append(f"{key} = {result.get(key)} is above the expected maximum {high}")
    return fails


def load(path):
    if os.path.isdir(path):
        logs = sorted(glob.glob(os.path.join(path, "status-*.log")), key=os.path.getsize)
        if not logs:
            sys.exit(f"no status-*.log in {path}")
        path = logs[-1]
    return open(path, encoding="utf-8", errors="ignore").read()


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if "--self-test" in argv:
        folder = os.path.join(root, "tests", "replay", "sample-native11")
        argv = [os.path.join(folder, "status-sample.txt"), "--expect", os.path.join(folder, "expect.json")]
    if not argv or argv[0].startswith("--"):
        sys.exit(__doc__)
    result = score(load(argv[0]))
    fails = []
    if "--expect" in argv:
        expect = json.load(open(argv[argv.index("--expect") + 1], encoding="utf-8"))
        fails = check(result, expect)
        for key, want in expect.get("exact", {}).items():
            if result.get(key) != want:
                fails.append(f"{key} = {result.get(key)}, expected exactly {want}")
    if "--json" in argv:
        print(json.dumps(result, indent=2))
    else:
        for k, v in result.items():
            print(f"{k:24s} {v}")
    for f in fails:
        print("FAIL: " + f)
    if "--expect" in argv and not fails:
        print("PASS")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
