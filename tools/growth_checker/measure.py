"""Turns a parsed status log into measurements. Each measurement is either
measured (with its unit, its basis and its limits) or unknown (with the reason).
A missing value is never zero and never a pass.

Bases: "counter" (the program counted it), "sampled" (estimated from the periodic
status samples, so only as fine as the sample period), "event" (frame-stamped
event log), "derived" (computed from other measurements), "observed" (a value
the log states, such as a setting).
"""
import re
import statistics

import statuslog

# What each provider reports. A provider is added by adding a row.
#   presented: frames the game presented while the program watched
#   scene:     frames the program judged to be a finished 3D scene (None: not logged apart from `presented`)
#   delivered: frames on which DLSS output was delivered
PROVIDERS = {
    "native11": {
        "api": "dx11", "route": "B",
        "presented": r"\bnative11 frames=(\d+)",
        "scene": r"\bnative11 frames=\d+ fired=(\d+)",
        "delivered": r"\bnative11_dlaa delivered=(\d+)",
        "jittered": r"\bviewport_jitter\{[^}]*?\bframes=(\d+)",
        "menu_draws": r"\bmenu\{draws=(\d+)",
        "preset": r"\bnative11_dlaa [^|]*?\bpreset=(\d+)",
        "sharpness": r"\bnative11_dlaa [^|]*?\bsharpness=(\d+)",
        "found_group": "pose", "reason_groups": {"split_rejected": "split_", "start_rejected": "start_"},
    },
    "dx12": {
        "api": "dx12", "route": "B",
        "presented": r"\bj2_coverage presents=(\d+)",
        "scene": None,
        "delivered": r"\bj2_coverage presents=\d+ covered=(\d+)",
        "jittered": r"\bj2_coverage [^\n]*?\bjittered=(\d+)",
        "menu_draws": r"\bmenu_gpu draws=(\d+)",
        "preset": r"^j2 on=[^\n]*?\bpreset=(\d+)",
        "sharpness": r"^j2 on=[^\n]*?\bsharpness=(\d+)",
        "found_group": None, "reason_groups": {},
        # The DX12 host states its misses as counters on the "j2 on=" line.
        "reason_line": (r"^j2 on=[^\n]*", ("no_selection", "no_scene_list", "no_state", "no_camera", "busy", "dlss_failed")),
    },
    # Route A: the game runs its own DLSS. Written from the format strings in the
    # source; not yet checked against a saved Route A run.
    "route_a": {
        "api": "dx12", "route": "A",
        "presented": r"\bengine_present seen=(\d+)",
        "scene": None,
        "delivered": None,
        "jittered": None,
        "menu_draws": r"\bmenu_gpu draws=(\d+)",
        "preset": None, "sharpness": None,
        "found_group": None, "reason_groups": {},
    },
}
# Counters that are successes inside pose{...}; the rest say why a frame got no camera.
FOUND_BY = {"native", "legacy", "generic", "split", "started", "continued", "no_inverse"}
LIMIT_SCENE = ("Frames the program did not judge to be scene may be menus, videos, loading or missed gameplay; "
               "this number cannot tell which.")
LIMIT_GAP = ("A gap over observed opportunities, estimated from the periodic status samples. Intended Off, menu and "
             "loading time are not told apart from unexpected misses.")


def measured(value, unit, basis, limits=None):
    m = {"state": "measured", "value": value, "unit": unit, "basis": basis}
    if limits:
        m["limits"] = limits
    return m


def unknown(why, unit=None):
    m = {"state": "unknown", "value": None, "why": why}
    if unit:
        m["unit"] = unit
    return m


def not_applicable(why):
    return {"state": "not_applicable", "value": None, "why": why}


def value(measurements, name):
    """The number of a measured value, else None. Callers must handle None."""
    m = measurements.get(name)
    return m["value"] if m and m["state"] == "measured" else None


def detect_provider(samples):
    """The provider whose delivered counter advanced most; failing that, whose
    presented counter advanced most. None when no provider's counters appear."""
    best = None
    for name, p in PROVIDERS.items():
        def advance(pattern):
            if not pattern:
                return 0
            xs = [x for x in statuslog.series(samples, pattern) if x is not None]
            return (max(xs) - min(xs)) if xs else -1
        key = (advance(p["delivered"]), advance(p["presented"]))
        if key[1] >= 0 and (best is None or key > best[0]):
            best = (key, name)
    if best is None and any("picture_mode " in s["text"] or "sl_boundary " in s["text"] for s in samples):
        return "route_a"
    return best[1] if best else None


def _segment(samples, a, b, pres, scene, deliv, period):
    """Counts and intervals of one session (samples a..b inclusive)."""
    out = {"first_sample": statuslog.clock(samples[a]["t"]), "last_sample": statuslog.clock(samples[b]["t"]),
           "samples": b - a + 1, "presented": None, "scene": None, "delivered": None,
           "run_seconds": 0.0, "unlogged_seconds": 0.0, "play_seconds": 0.0, "delivering_seconds": 0.0,
           "longest_gap_seconds": 0.0, "longest_gap_window_seconds": 0.0, "longest_gap_started": None,
           "first_delivery_after_seconds": None, "fps_play": [], "fps_delivering": [], "ms_play": []}

    def total(c):
        xs = [c[i] for i in range(a, b + 1) if c and c[i] is not None]
        return xs[-1] - xs[0] if len(xs) >= 2 else None
    out["presented"], out["scene"], out["delivered"] = total(pres), total(scene), total(deliv)
    opp_series = scene if scene else pres  # opportunities for delivery, as far as the log can say
    gap = span = 0.0
    gap_start = None
    first_play = None
    for i in range(a, b):
        dt = (samples[i + 1]["t"] - samples[i]["t"]) % 86400
        if period and dt > 6 * period:  # the log was not being written (suspended, alt-tabbed out, clock change)
            out["unlogged_seconds"] += dt
            gap = span = 0.0
            continue
        out["run_seconds"] += dt
        if not opp_series or opp_series[i] is None or opp_series[i + 1] is None:
            continue
        opp = opp_series[i + 1] - opp_series[i]
        if opp <= 0:
            gap = span = 0.0
            continue
        nxt = samples[i + 1]
        out["play_seconds"] += dt
        if nxt["fps"] > 0:
            out["fps_play"].append(nxt["fps"])
        if nxt["frame_ms"]:
            out["ms_play"].append(nxt["frame_ms"])
        if first_play is None:
            first_play = out["run_seconds"] - dt
        if not deliv or deliv[i] is None or deliv[i + 1] is None:
            continue
        got = deliv[i + 1] - deliv[i]
        if got > 0 and out["first_delivery_after_seconds"] is None:
            out["first_delivery_after_seconds"] = out["run_seconds"] - dt - first_play
        missed = max(0, opp - got) / opp
        if missed > 0.02:
            if gap == 0:
                gap_start = samples[i]["t"]
            gap += dt * missed
            span += dt
            if gap > out["longest_gap_seconds"]:
                out["longest_gap_seconds"], out["longest_gap_window_seconds"] = gap, span
                out["longest_gap_started"] = statuslog.clock(gap_start)
        else:
            gap = span = 0.0
        if got >= 0.98 * opp:
            out["delivering_seconds"] += dt
            if nxt["fps"] > 0:
                out["fps_delivering"].append(nxt["fps"])
    return out


def _counter_delta(samples, spans, pattern):
    """Sum of a counter's advance inside each session; None when it never appears."""
    if not pattern:
        return None
    c = statuslog.series(samples, pattern)
    total, seen = 0, False
    for a, b in spans:
        xs = [c[i] for i in range(a, b + 1) if c[i] is not None]
        if len(xs) >= 2:
            total += xs[-1] - xs[0]
            seen = True
    return total if seen else None


def _group_delta(samples, spans, name):
    """Advance of every counter in `name{...}` summed over sessions; None if absent."""
    total, seen = {}, False
    for a, b in spans:
        first = last = None
        for i in range(a, b + 1):
            g = statuslog.group(samples[i]["text"], name)
            if g is not None:
                first = g if first is None else first
                last = g
        if first is not None and last is not None:
            seen = True
            for k, v in last.items():
                total[k] = total.get(k, 0) + v - first.get(k, 0)
    return total if seen else None


def measure(parsed, provider=None):
    """-> {"provider", "api", "route", "sessions": [...], "measurements": {...}, "observed": {...}}

    `provider` forces a reader (a case file names it); otherwise it is detected.
    A log with no samples or no known counters returns {"error": ...}: a reading
    failure is its own outcome, not a result.
    """
    samples = parsed["samples"]
    if not samples:
        return {"error": "no status samples in the log"}
    detected = detect_provider(samples)
    name = provider or detected
    if name not in PROVIDERS:
        return {"error": "no known provider counters in the log"}
    p = PROVIDERS[name]
    pres = statuslog.series(samples, p["presented"]) if p["presented"] else None
    scene = statuslog.series(samples, p["scene"]) if p["scene"] else None
    deliv = statuslog.series(samples, p["delivered"]) if p["delivered"] else None
    if pres is None or all(x is None for x in pres):
        return {"error": f"the log has no '{name}' counters", "detected_provider": detected}
    # A counter the log never states is absent, not zero.
    scene = None if scene is None or all(x is None for x in scene) else scene
    deliv = None if deliv is None or all(x is None for x in deliv) else deliv
    spans = statuslog.sessions(samples, [c for c in (pres, scene, deliv) if c])
    steps = [(samples[i + 1]["t"] - samples[i]["t"]) % 86400 for i in range(len(samples) - 1)]
    steps = [s for s in steps if s > 0]
    period = statistics.median(steps) if steps else None
    segs = [_segment(samples, a, b, pres, scene, deliv, period) for a, b in spans]

    def add(key):
        xs = [s[key] for s in segs if s[key] is not None]
        return sum(xs) if xs else None
    presented, scenes, delivered = add("presented"), add("scene"), add("delivered")
    play, delivering = add("play_seconds") or 0.0, add("delivering_seconds") or 0.0
    fps_play = sorted(x for s in segs for x in s["fps_play"])
    fps_on = sorted(x for s in segs for x in s["fps_delivering"])
    ms_play = sorted(x for s in segs for x in s["ms_play"])
    worst = max(segs, key=lambda s: s["longest_gap_seconds"])
    m = {}
    count = lambda v, what: measured(v, "frames", "counter") if v is not None else unknown(what, "frames")
    m["frames_presented"] = count(presented, "the log has no presented-frame counter")
    m["frames_scene"] = count(scenes, f"the {name} provider does not log scene frames apart from presented frames")
    m["frames_delivered"] = count(delivered, "the log has no delivered-frame counter"
                                  if p["delivered"] else "this route delivers through the game's own DLSS; not counted here")

    def share(part, whole, limits=None, why="a count it needs is unknown"):
        if part is None or whole is None:
            return unknown(why, "percent")
        if whole <= 0:
            return unknown("no frames to divide by", "percent")
        return measured(round(100.0 * part / whole, 2), "percent", "derived", limits)
    m["scene_share_of_presented"] = share(scenes, presented, LIMIT_SCENE)
    m["delivered_share_of_scene"] = share(delivered, scenes)
    m["delivered_share_of_presented"] = share(delivered, presented, LIMIT_SCENE)
    m["run_seconds"] = measured(round(add("run_seconds") or 0.0, 1), "seconds", "sampled")
    m["unlogged_seconds"] = measured(round(add("unlogged_seconds") or 0.0, 1), "seconds", "sampled",
                                     "Time with no status samples (suspended or not running); not counted as play.")
    m["sample_period_seconds"] = measured(period, "seconds", "sampled") if period else unknown("fewer than two samples")
    has_opportunities = any(s["play_seconds"] > 0 for s in segs)
    if has_opportunities:
        m["play_seconds"] = measured(round(play, 1), "seconds", "sampled",
                                     "Time in which the program saw delivery opportunities (scene frames, or presented "
                                     "frames where scene frames are not logged).")
    else:
        m["play_seconds"] = unknown("no interval with delivery opportunities", "seconds")
    if deliv is not None and has_opportunities:
        m["longest_gap_seconds"] = measured(round(worst["longest_gap_seconds"], 1), "seconds", "sampled", LIMIT_GAP)
        m["longest_gap_window_seconds"] = measured(round(worst["longest_gap_window_seconds"]), "seconds", "sampled", LIMIT_GAP)
        m["longest_gap_started"] = measured(worst["longest_gap_started"], "clock", "sampled")
        m["not_delivering_share_of_play"] = measured(round(100.0 * (play - delivering) / play, 2), "percent", "sampled",
                                                     "Share of play time in samples where under 98% of opportunities got DLSS.")
        firsts = [s["first_delivery_after_seconds"] for s in segs if s["first_delivery_after_seconds"] is not None]
        m["time_to_first_delivery_seconds"] = (measured(round(firsts[0], 1), "seconds", "sampled",
                                                        "From the first sample with opportunities to the first with a delivery.")
                                               if firsts else unknown("nothing was delivered", "seconds"))
    else:
        why = "no delivered-frame counter" if deliv is None else "no interval with delivery opportunities"
        for key in ("longest_gap_seconds", "longest_gap_window_seconds", "longest_gap_started",
                    "not_delivering_share_of_play", "time_to_first_delivery_seconds"):
            m[key] = unknown(why)

    def middle(xs, unit, limits=None, why="no fps samples during play"):
        return measured(xs[len(xs) // 2], unit, "sampled", limits) if xs else unknown(why, unit)
    m["fps_play_median"] = middle(fps_play, "fps", "Over every sample with delivery opportunities, delivering or not.")
    m["fps_play_lowest"] = measured(fps_play[0], "fps", "sampled") if fps_play else unknown("no fps samples during play", "fps")
    m["frame_ms_play_median"] = middle(ms_play, "ms", why="the log has no frame_ms on its samples")
    m["fps_median_while_delivering"] = middle(fps_on, "fps", "Only samples where at least 98% of opportunities got DLSS; "
                                              "read it beside not_delivering_share_of_play.", "no sample was delivering")
    m["fps_lowest_while_delivering"] = measured(fps_on[0], "fps", "sampled") if fps_on else unknown("no sample was delivering", "fps")

    jit = _counter_delta(samples, spans, p["jittered"])
    m["frames_jittered"] = count(jit, "the log has no jitter counter")
    menu = _counter_delta(samples, spans, p["menu_draws"])
    m["menu_draws"] = (measured(menu, "draws", "counter", "The Home menu was drawn this many times; says nothing about its controls.")
                       if menu is not None else unknown("the log has no menu draw counter", "draws"))

    found = why_missed = None
    if p["found_group"]:
        g = _group_delta(samples, spans, p["found_group"])
        if g is not None:
            found = {k: v for k, v in g.items() if k in FOUND_BY and v > 0}
            why_missed = {k: v for k, v in g.items() if k not in FOUND_BY and v > 0}
    for group_name, prefix in p["reason_groups"].items():
        g = _group_delta(samples, spans, group_name)
        if g is not None:
            why_missed = why_missed or {}
            why_missed.update({prefix + k: v for k, v in g.items() if v > 0})
    if p.get("reason_line"):
        line_rx, keys = p["reason_line"]
        totals, seen = {}, False
        for a, b in spans:
            first = last = None
            for i in range(a, b + 1):
                lm = re.search(line_rx, samples[i]["text"], re.M)
                if lm:
                    kv = {k: int(v) for k, v in re.findall(r"\b(\w+)=(\d+)\b", lm[0]) if k in keys}
                    first = kv if first is None else first
                    last = kv
            if first is not None:
                seen = True
                for k, v in last.items():
                    totals[k] = totals.get(k, 0) + v - first.get(k, 0)
        if seen:
            why_missed = {k: v for k, v in totals.items() if v > 0}
    m["camera_found_by"] = (measured(found, "frames per rule", "counter",
                                     "'native' counts every camera-relative pose, so it overlaps 'split'.")
                            if found is not None else unknown(f"the {name} provider does not log which rule found the camera"))
    m["missed_because"] = (measured(why_missed, "frames per reason", "counter")
                           if why_missed is not None else unknown("the log gives no reasons for missed frames (a telemetry gap)"))

    observed = {}
    # Settings count only once DLSS is delivering: the zeros a log shows while the
    # program is still starting are not settings anyone chose.
    live = [i for i in range(len(samples)) if deliv and deliv[i]] or list(range(len(samples)))
    for key in ("preset", "sharpness"):
        if p[key]:
            c = statuslog.series(samples, p[key])
            xs = [c[i] for i in live if c[i] is not None]
            if xs:
                observed[key] = {"last": xs[-1], "distinct": sorted(set(xs))}
    layout = None
    for s in samples:
        lm = re.search(r"\brelative_layout\{(\w+)", s["text"])
        if lm:
            layout = lm[1] if layout in (None, lm[1]) else "seed and learned"
    if layout:
        observed["relative_layout"] = layout
    sessions = [{k: (round(v, 1) if isinstance(v, float) else v) for k, v in s.items()
                 if k not in ("fps_play", "fps_delivering", "ms_play")} for s in segs]
    return {"provider": name, "detected_provider": detected, "api": p["api"], "route": p["route"],
            "sessions": sessions, "measurements": m, "observed": observed}
