"""A3. Credit observed reuse and report where a title still supplies knowledge."""
import json

from ._evidence import LIVE, STRONG_BUILD_SOURCES, build, build_source, manifest, number, same_build, status_hash


def _pattern(result):
    # Don't turn the last sample of a mixed run into a run-wide pattern.
    sources = {"scene_placement": "scene_placement", "depth_convention": "depth_reversed",
               "jitter_route": "viewport_jitter_mode"}
    observations = result.get("observed", {})
    mixed = {name for name, source in sources.items() if isinstance(observations.get(source), dict)
             and len(observations[source].get("distinct", [])) > 1}
    pattern = {k: v["value"] for k, v in result.get("patterns", {}).items()
               if k not in mixed | {"api", "provider"} and v.get("state") == "observed"
               and isinstance(v.get("value"), (str, bool, int))}
    admission = result.get("patterns", {}).get("camera_admission", {})
    if admission.get("state") == "observed" and isinstance(admission.get("value"), dict):
        methods = [name for name, frames in admission["value"].items() if frames > 0]
        if len(methods) == 1:
            pattern["camera_admission"] = methods[0]
    return pattern


def _live_success(result, case):
    receipt = manifest(result)
    if (result.get("status") != "scored" or receipt.get("evidence_type") != LIVE
            or receipt.get("case") != case.get("id") or not build(result)
            or build_source(result) not in STRONG_BUILD_SOURCES):
        return False
    if any(case.get(k) and case[k] != result.get(k) for k in ("route", "provider", "api")):
        return False
    job = "dlaa_reaches_picture" if case.get("route") == "B" else "mode_changes_render_size"
    verdict = result.get("jobs", {}).get(job, {})
    if verdict.get("state") != "verified" or any(j.get("state") == "failed" for j in result.get("jobs", {}).values()):
        return False
    if case.get("route") == "B":
        return verdict.get("source") == "owner_verdict" and bool(number(result, "frames_delivered"))
    return True


def _engine(case):
    engine = case.get("engine") or {}
    return engine.get("name") if engine.get("source") else None


def credit(results_by_case, cases):
    credits, seen_credits = [], set()
    combinations, title_specific, shared = {}, {}, {}

    def add(kind, case, pattern, because):
        key = (kind, case, json.dumps(pattern, sort_keys=True))
        if key not in seen_credits:
            credits.append({"kind": kind, "case": case, "pattern": pattern, "because": because})
            seen_credits.add(key)

    for case_id, results in sorted(results_by_case.items()):
        case = cases.get(case_id)
        if not case:
            continue
        seen = set()
        # Prefer the most recent scoring of the same log; deduplicate by hash
        # even if an imported record used a different evidence ID.
        for result in reversed(results):
            receipt = manifest(result)
            key = status_hash(result) or receipt.get("evidence_id")
            if not key or key in seen:
                continue
            seen.add(key)
            if (result.get("status") != "scored" or receipt.get("case") != case_id
                    or receipt.get("evidence_type") != LIVE):
                continue
            deps = result.get("dependencies_in_use", [])
            # A rule may report several counters over the same frames. The
            # maximum is a lower bound on unique frames for a variant per run.
            titled = {}
            assisted = False
            shared_uses = set()
            for dep in deps:
                frames = dep.get("frames")
                if not isinstance(frames, int) or isinstance(frames, bool) or frames <= 0:
                    continue
                if dep.get("kind") == "title_specific":
                    name = dep["variant"]
                    titled[name] = max(frames, titled.get(name, 0))
                if dep.get("kind") != "shared_rule" or not dep.get("source"):
                    assisted = True
                elif dep["variant"]:
                    shared_uses.add(dep["variant"])
            for name, frames in titled.items():
                row = title_specific.setdefault(name, {"variant": name, "cases": set(), "frames": 0})
                row["cases"].add(case_id)
                row["frames"] += frames

            if not _live_success(result, case):
                continue
            pattern = _pattern(result)
            combo = {**pattern, "api": result.get("api"), "provider": result.get("provider")}
            if len(pattern) >= 2:
                pattern_key = json.dumps(combo, sort_keys=True)
                combinations.setdefault(pattern_key, {"patterns": combo, "cases": set()})["cases"].add(case_id)
            first = receipt.get("first_contact") or {}
            if (not assisted and first.get("zero_change") is True and first.get("source")
                    and same_build(first.get("build"), build(result))):
                add("zero_change_first_contact", case_id, combo,
                    "The recorded first run succeeded on the existing build without a game-specific change.")

            for dep in deps:
                frames = dep.get("frames")
                if not isinstance(frames, int) or isinstance(frames, bool) or frames <= 0 or not dep.get("source"):
                    continue
                variant = dep["variant"]
                kind = dep.get("kind")
                if kind == "shared_rule_with_adapter_hint":
                    add("adapter_hint", case_id, {"variant": variant},
                        "The shared rule validated a layout supplied by an adapter.")
            # The counters do not associate simultaneous rule uses with one
            # frame. An adapter-dependent run cannot establish unaided reuse.
            if not assisted:
                for variant in sorted(shared_uses):
                    shared.setdefault(variant, []).append((case_id, pattern, build(result)))

    for variant, uses in sorted(shared.items()):
        for case_id, pattern, commit in uses:
            for other, other_pattern, other_commit in uses:
                case, peer = cases[case_id], cases[other]
                if case["game"] == peer["game"] or not same_build(commit, other_commit):
                    continue
                engine, other_engine = _engine(case), _engine(peer)
                different_patterns = any(pattern[k] != other_pattern[k] for k in pattern.keys() & other_pattern.keys())
                if engine and other_engine and engine != other_engine:
                    add("independent_engine", case_id, {"variant": variant, "peer": other},
                        "The same build used this shared rule successfully in a game with a different declared engine.")
                if different_patterns:
                    add("independent_pattern", case_id, {"variant": variant, "peer": other},
                        "The same build used this shared rule successfully in a game with a different observed rendering pattern.")

    return {"credits": sorted(credits, key=lambda c: (c["case"], c["kind"], json.dumps(c["pattern"], sort_keys=True))),
            "combinations": [{**row, "cases": sorted(row["cases"])} for _, row in sorted(combinations.items())],
            "title_specific_in_use": [{**row, "cases": sorted(row["cases"])} for _, row in sorted(title_specific.items())]}
