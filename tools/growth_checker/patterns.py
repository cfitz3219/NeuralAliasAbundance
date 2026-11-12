"""Rendering patterns a run showed, and the rules and title-specific code it used.

Only what the log states is recorded; the rest is unknown. The admission method
is kept apart from where a layout came from: a shared rule that validates a
seeded layout is validation with an adapter's hint, not discovery.
"""
from measure import value

# pose{...} counters of the native DX11 provider -> (admission method, variant in docs/VARIANTS.md, kind)
CAMERA_RULES = {
    "generic": ("inverse pair", None, "shared_rule"),
    "split": ("camera-relative, position witnessed", "camera/relative", "shared_rule"),
    "started": ("no inverse, started", "camera/no-inverse", "shared_rule"),
    "continued": ("no inverse, continued", "camera/no-inverse", "shared_rule"),
    "legacy": ("packed layout", "camera11/packed", "title_specific"),
}
UNKNOWN = lambda why: {"state": "unknown", "why": why}
KNOWN = lambda v, source: {"state": "observed", "value": v, "source": source}


def describe(provider, api, m, observed):
    """-> (patterns, dependencies_in_use)"""
    patterns = {
        "api": KNOWN(api, "provider that counted the frames"),
        "provider": KNOWN(provider, "provider that counted the frames"),
        "depth_convention": UNKNOWN("not logged yet"),
        "jitter_route": UNKNOWN("not logged yet"),
        "motion_source": UNKNOWN("not logged yet"),
        "hud_composition": UNKNOWN("not logged yet"),
        "presentation": UNKNOWN("not logged yet"),
    }
    stated = lambda key: (observed.get(key) or {}).get("last")
    if stated("scene_placement"):
        # Where DLAA goes relative to the game's own scene work: between command lists, inside the scene list, or at an upscaler call.
        patterns["scene_placement"] = KNOWN(stated("scene_placement"), "j2_route route=")
    if stated("depth_reversed") is not None:
        patterns["depth_convention"] = KNOWN("reversed" if stated("depth_reversed") == "1" else "forward", "j2 reversed=")
    if stated("viewport_jitter_mode") is not None:
        patterns["jitter_route"] = KNOWN("camera matrix" if stated("viewport_jitter_mode") == "0" else "viewport", "viewport_jitter mode=")
    deps = []
    found = value(m, "camera_found_by")
    if found is None:
        patterns["camera_admission"] = UNKNOWN("this provider does not log which rule found the camera")
        patterns["camera_layout_from"] = UNKNOWN("not logged")
        return patterns, deps
    methods = {}
    for key, frames in found.items():
        if key in CAMERA_RULES:
            method, variant, kind = CAMERA_RULES[key]
            methods[method] = methods.get(method, 0) + frames
            if variant:
                deps.append({"variant": variant, "kind": kind, "frames": frames, "source": f"pose{{{key}}}"})
    patterns["camera_admission"] = KNOWN(methods, "pose{...} counters")
    layout = observed.get("relative_layout")
    relative = any(d["variant"] == "camera/relative" for d in deps)
    if not relative:
        patterns["camera_layout_from"] = {"state": "not_applicable", "why": "no camera-relative pose in this run"}
    elif layout is None:
        patterns["camera_layout_from"] = UNKNOWN("this build did not log whether the relative layout was seeded or learned")
        for d in deps:
            if d["variant"] == "camera/relative":
                d["kind"] = "provenance_unknown"  # fixed places, a seed or a learned layout: the log cannot say
    else:
        patterns["camera_layout_from"] = KNOWN(layout, "relative_layout{...}")
        for d in deps:
            if d["variant"] == "camera/relative" and "seed" in layout:
                d["kind"] = "shared_rule_with_adapter_hint"  # the WWII seed layout supplied the places
    return patterns, deps
