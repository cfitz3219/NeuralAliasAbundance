"""A2. Catch misleading gains and evidence that cannot support a verdict."""
from ._evidence import build, manifest, measurement, number, same_evidence, slack, tolerance_for


def check(baseline, candidate, case):
    out = []

    def flag(guard, severity, message, *fields):
        out.append({"guard": guard, "severity": severity, "message": message, "fields": list(fields)})

    receipt = manifest(candidate)
    commit = build(candidate)
    evidence_prefix = (receipt.get("evidence_id") or "").partition("-")[0]
    if (candidate.get("status") == "refused"
            or commit and evidence_prefix and evidence_prefix != commit[:8]):
        flag("wrong_build_evidence", "blocks", "The evidence does not identify the claimed build.")
    if same_evidence(baseline, candidate):
        old_scorer = manifest(baseline).get("scorer", {}).get("version")
        new_scorer = receipt.get("scorer", {}).get("version")
        message = ("The same saved run was scored again with a different reader." if old_scorer != new_scorer
                   else "The same saved run appears on both sides of the comparison.")
        flag("reused_evidence", "blocks", message)
    if len(candidate.get("sessions", [])) > 1:
        flag("counter_reset", "warns", "The log contains counter resets. Only measurements within each session were counted.",
             "frames_presented", "frames_scene", "frames_delivered")

    def delta(name):
        old, new = number(baseline, name), number(candidate, name)
        if old is None or new is None:
            return 0
        change = new - old
        margin = slack(old, tolerance_for(name, baseline, candidate))
        return change if abs(change) > margin else 0

    scenes = "scene_share_of_presented"
    per_scene = "delivered_share_of_scene"
    per_present = "delivered_share_of_presented"
    fps = "fps_play_median"
    conditional = "fps_median_while_delivering"
    missed = "not_delivering_share_of_play"
    if delta(scenes) < 0:
        severity = "blocks" if delta(per_present) < 0 else "warns"
        flag("fewer_scenes", severity,
             "The run recognised a smaller share of presents as scenes. Check delivery across all presents.",
             scenes, per_scene, per_present)
    if delta(fps) > 0 and (delta(per_present) < 0 or delta(per_scene) < 0):
        flag("speed_with_lost_coverage", "blocks",
             "Frame rate rose while DLAA covered fewer frames. The speed gain cannot count as an improvement.",
             fps, per_present, per_scene)
    if (delta(conditional) > 0 and delta(fps) <= 0
            or (number(candidate, missed) or 0) > 1 and number(candidate, conditional) is not None):
        flag("conditional_fps", "warns",
             "FPS while delivering excludes low-delivery intervals. Use the whole-play measurement for performance.",
             conditional, fps, missed)

    budget = case.get("budget") or {}
    for setting, field in (("fps_min", fps), ("longest_gap_seconds_max", "longest_gap_seconds")):
        if budget.get(setting) is not None and number(candidate, field) is None:
            flag("missing_budget_measure", "warns", f"The run did not measure {field} needed for its budget.", field)

    for field in ("longest_gap_seconds", "longest_gap_window_seconds"):
        old, new = measurement(baseline, field), measurement(candidate, field)
        if old.get("state") == new.get("state") == "measured" and (old.get("basis") == "sampled") != (new.get("basis") == "sampled"):
            flag("gap_basis_changed", "warns", "The gap changed between a sampled estimate and a direct measure.", field)
        elif new.get("state") == "measured" and new.get("basis") == "sampled":
            flag("sampled_gap", "warns", "The reported gap is an estimate over sampled opportunities.", field)
    return out
