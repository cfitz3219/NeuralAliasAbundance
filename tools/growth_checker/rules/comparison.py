"""A1. Decide which recorded results can be compared and when a floor can move."""
from ._evidence import (LIVE, STRONG_BUILD_SOURCES, build, build_source, intervals,
                        manifest, measurement, number, observed, same_build,
                        same_evidence, slack, status_hash, tolerance_for)
from .guards import check

# These measures can improve with a stronger result. Raw frame totals describe
# the amount of evidence; conditional FPS does not describe the whole session.
PROMOTABLE = {
    "delivered_share_of_scene", "delivered_share_of_presented",
    "longest_gap_seconds", "longest_gap_window_seconds",
    "not_delivering_share_of_play", "fps_play_median", "frame_ms_play_median",
}


def judge(baseline, candidate, case):
    reasons, limits = [], []
    for label, result in (("Baseline", baseline), ("Candidate", candidate)):
        receipt = manifest(result)
        if result.get("status") != "scored":
            reasons.append(f"{label} has no scored run.")
        if receipt.get("case") != case.get("id"):
            reasons.append(f"{label} is recorded under a different case.")
        profile = (receipt.get("session") or {}).get("profile_hint")
        if profile and case.get("profile") and profile != case["profile"]:
            reasons.append(f"{label} results came from a different profile.")
        for key in ("route", "api", "provider"):
            expected, actual = case.get(key), result.get(key)
            if expected and actual and expected != actual:
                reasons.append(f"{label} {key} is {actual}, expected {expected}.")
            elif not actual:
                limits.append(f"{label} {key} is unknown.")
        if build_source(result) not in STRONG_BUILD_SOURCES or not build(result):
            limits.append(f"{label} build attribution is incomplete.")
        if intervals(result) < 3:
            limits.append(f"{label} has fewer than three measured intervals.")
        if len(result.get("sessions", [])) > 1 or (number(result, "unlogged_seconds") or 0) > 0:
            limits.append(f"{label} contains separate sessions or unlogged time.")

    first, second = manifest(baseline), manifest(candidate)
    if first.get("evidence_type") != second.get("evidence_type"):
        reasons.append("The runs use different kinds of evidence.")
    elif second.get("evidence_type") != LIVE:
        limits.append("This comparison describes automated evidence.")
    if same_evidence(baseline, candidate):
        reasons.append("Both results come from the same saved evidence.")

    scene = (case.get("scene") or {}).get("id")
    scenes = [observed(r, "scene_id") for r in (baseline, candidate)]
    if (all(s is not None for s in scenes) and scenes[0] != scenes[1]
            or scene and any(s is not None and s != scene for s in scenes)):
        reasons.append("The saved runs identify different scenes.")
    elif not scene or None in scenes:
        limits.append("The same gameplay scene is not confirmed in both runs.")

    for setting, expected in sorted((case.get("settings") or {}).items()):
        values = [observed(r, setting) for r in (baseline, candidate)]
        changes = [r.get("observed", {}).get(setting) for r in (baseline, candidate)]
        if (any(v is not None and v != expected for v in values)
                or any(v != expected for entry in changes if isinstance(entry, dict)
                       for v in entry.get("distinct", []))):
            reasons.append(f"An observed {setting} differs from the case setting.")
        elif None in values:
            limits.append(f"The case setting {setting} is not confirmed in both runs.")

    # Optional observations from the run card. Until available, keep historical
    # runs usable for reporting conspicuous failures, with their scope visible.
    for key, label in (("test_environment", "hardware and display settings"),
                       ("play_intervals", "comparable gameplay intervals")):
        values = [observed(r, key) for r in (baseline, candidate)]
        if key == "play_intervals":
            if any(v is not True for v in values):
                limits.append("Warm-up, menu, intentional Off and cut intervals are not independently separated.")
        elif all(v is not None for v in values) and values[0] != values[1]:
            reasons.append(f"The {label} differ.")
        elif None in values:
            limits.append(f"The {label} are not confirmed in both runs.")

    tolerance = {name: tolerance_for(name, baseline, candidate)
                 for name in sorted(candidate.get("measurements", {})) if number(candidate, name) is not None}
    verdict = "not_comparable" if reasons else "comparable_with_limits" if limits else "comparable"
    answer = {"verdict": verdict, "limits": limits, "reasons": reasons, "tolerance": tolerance}
    # These describe the comparison, not the candidate alone. Keep the reason
    # alongside the decision so floor_action need not reload baseline evidence.
    for flag in check(baseline, candidate, case):
        if flag["severity"] == "blocks" or flag["guard"] in {"fewer_scenes", "gap_basis_changed"}:
            answer["floor_block"] = flag["message"]
            break
    return answer


def floor_action(case, floor, candidate, comparison, earlier_proposal):
    def answer(action, reason, values=None):
        return {"action": action, "floor": values, "reason": reason}

    existing = (floor or {}).get("floor") or {}
    if not existing:
        return answer("needs_owner", "The case needs an accepted initial floor.")
    if (comparison.get("verdict") != "comparable" or manifest(candidate).get("evidence_type") != LIVE
            or build_source(candidate) not in STRONG_BUILD_SOURCES or not status_hash(candidate)):
        return answer("keep", "Floor changes need matched, attributable gameplay evidence.")
    if (floor or {}).get("case") != case.get("id"):
        return answer("needs_owner", "The floor belongs to a different case.")
    if comparison.get("floor_block"):
        return answer("keep", comparison["floor_block"])
    if any(j.get("state") == "failed" for j in candidate.get("jobs", {}).values()):
        return answer("keep", "A required job failed in this run.")
    if case.get("route") == "B" and not number(candidate, "frames_delivered"):
        return answer("keep", "There is no delivered DLAA to support a higher floor.")
    fps_min = (case.get("budget") or {}).get("fps_min")
    fps = number(candidate, "fps_play_median")
    if fps_min is not None and (fps is None or fps < fps_min):
        return answer("keep", "The gameplay performance budget has not been met.")
    gap_max = (case.get("budget") or {}).get("longest_gap_seconds_max")
    gap = number(candidate, "longest_gap_seconds")
    if gap_max is not None and (gap is None or gap > gap_max):
        return answer("keep", "The gameplay continuity budget has not been met.")

    updated = {}
    for name, entry in sorted(existing.items()):
        value = number(candidate, name)
        if value is None:
            return answer("keep", f"The run is missing the floor measurement {name}.")
        target, kind = entry["value"], entry["kind"]
        margin = slack(target, comparison.get("tolerance", {}).get(name, {}))
        better = value - target if kind == "min" else target - value
        if better < -margin:
            return answer("keep", f"The run fell below its existing {name} floor.")
        if name in PROMOTABLE and better > margin and measurement(candidate, name).get("basis") != "sampled":
            updated[name] = {"kind": kind, "value": value}
        elif name in {"fps_play_median", "frame_ms_play_median"} and better > margin:
            updated[name] = {"kind": kind, "value": value}

    if earlier_proposal:
        prior = earlier_proposal.get("from") or ""
        evidence = manifest(candidate).get("evidence_id") or ""
        # The caller identifies the scheduled confirmation by its saved
        # evidence ID. Neither re-scoring nor a repeat on another build counts.
        source_build, _, source_digest = prior.partition("-")
        if (not source_digest or not same_build(source_build, build(candidate))
                or prior == evidence or (status_hash(candidate) or "").startswith(source_digest)
                or earlier_proposal.get("awaits") != evidence):
            return answer("keep", "The existing proposal is waiting for its scheduled independent confirmation run.")
        proposed = earlier_proposal.get("floor") or {}
        changed = {name: entry for name, entry in proposed.items() if existing.get(name) != entry}
        if not changed or any(name not in updated or entry["kind"] != updated[name]["kind"]
                              or (updated[name]["value"] < entry["value"] if entry["kind"] == "min"
                                  else updated[name]["value"] > entry["value"])
                              for name, entry in changed.items()):
            return answer("keep", "The scheduled run did not confirm the proposed floor.")
        return answer("confirm", "An independent scheduled run on the same build met the proposed floor.",
                      {**existing, **changed})

    if updated:
        return answer("propose", "This run supports a higher floor. Confirm it on the next scheduled run.",
                      {**existing, **updated})
    return answer("keep", "The floor measurements show no repeatable improvement yet.")
