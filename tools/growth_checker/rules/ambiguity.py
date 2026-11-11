"""A4. Summarize evidence without carrying a live verdict across builds."""
from ._evidence import LIVE, same_build, slack


def job_state(pieces):
    if not pieces:
        return {"state": "unknown", "because": "This job has no recorded evidence."}
    builds = [p["build"] for p in pieces if p.get("build")]
    if builds and any(not same_build(max(builds, key=len), b) for b in builds):
        return {"state": "unknown", "because": "These pieces refer to different builds. Select the build to judge."}
    if not builds:
        return {"state": "unknown", "because": "The evidence does not identify a build for this job."}
    current = [p for p in pieces if p.get("build")]
    informative = {"verified", "failed", "not_applicable"}
    for kind in (LIVE, "replay", "fixture_test", "synthetic"):
        states = {p.get("state") for p in current
                  if p.get("evidence_type") == kind and p.get("state") in informative}
        if len(states) > 1:
            return {"state": "unknown", "because": f"Evidence of type {kind} disagrees on this job."}
        if states:
            state = states.pop()
            if kind != LIVE:
                return {"state": "untested", "because": f"The {kind} result was {state}. Gameplay verification is still open."}
            return {"state": state, "because": f"The result comes from {kind} evidence for this build."}
    state = "unknown" if any(p.get("state") == "unknown" for p in current) else "untested"
    return {"state": state, "because": "The recorded evidence does not settle this job."}


# Meaningful comparison axes. Duration and counts describe sample size, and
# conditional FPS and sampled gap estimates never earn improvement by themselves.
DIRECTIONS = {
    "fps_play_median": 1,
    "frame_ms_play_median": -1,
    "scene_share_of_presented": 1,
    "delivered_share_of_scene": 1,
    "delivered_share_of_presented": 1,
    "not_delivering_share_of_play": -1,
}


def case_verdict(comparison, measure_changes, jobs_before, jobs_after, guards):
    def answer(word, because):
        return {"answer": word, "because": because}

    before, after = jobs_before or {}, jobs_after or {}
    verdict = comparison.get("verdict")
    if any(g["guard"] in {"wrong_build_evidence", "reused_evidence"} for g in guards):
        return answer("not proven", [g["message"] for g in guards
                                     if g["guard"] in {"wrong_build_evidence", "reused_evidence"}])
    failed = sorted(job for job, detail in after.items() if detail.get("state") == "failed")
    valid = verdict in {"comparable", "comparable_with_limits"}
    if valid:
        excluded = {field for g in guards if g["guard"] == "gap_basis_changed" for field in g["fields"]}
        below = [f"{name} fell below its accepted floor."
                 for name, row in sorted(measure_changes.items())
                 if row.get("against_floor") == "below" and name not in excluded]
        lost = [f"{job} was verified and now fails." for job in failed if before.get(job, {}).get("state") == "verified"]
        blocked = [g["message"] for g in guards if g["severity"] == "blocks"]
        if below or lost or blocked:
            return answer("receded", below + lost + blocked)
    if failed:
        return answer("failing", [f"{job} failed in this run." for job in failed])
    if not valid:
        return answer("not proven", comparison.get("reasons") or ["A comparable baseline is needed."])

    unknown = sorted(name for name, row in measure_changes.items() if row.get("against_floor") == "unknown")
    missing_jobs = sorted(job for job in set(before) | set(after)
                          if after.get(job, {}).get("state") not in {"verified", "not_applicable"})
    unresolved = [g["message"] for g in guards
                  if g["guard"] not in {"sampled_gap", "conditional_fps"}]
    if unknown or missing_jobs or not after or verdict != "comparable" or unresolved:
        reasons = list(comparison.get("limits") or [])
        reasons += [f"No verified result for {job}." for job in missing_jobs]
        reasons += [f"No comparable measurement for the {name} floor." for name in unknown]
        reasons += unresolved
        return answer("not proven", reasons or ["The required gameplay jobs are not verified."])

    gains = [f"{job} is now verified." for job, j in sorted(after.items())
             if j.get("state") == "verified" and before.get(job, {}).get("state") in {"failed", "untested", "unknown"}]
    losses = []
    compared = False
    for name, direction in DIRECTIONS.items():
        row = measure_changes.get(name, {})
        old, change = row.get("baseline"), row.get("change")
        if old is None or change is None:
            continue
        compared = True
        margin = slack(old, comparison.get("tolerance", {}).get(name, {}))
        if direction * change > margin:
            gains.append(f"{name} improved beyond normal variation.")
        elif direction * change < -margin:
            losses.append(f"{name} worsened beyond normal variation.")
    if losses:
        return answer("not proven", losses + gains)
    if gains:
        return answer("grew", gains)
    if compared or any(row.get("against_floor") == "holds" for row in measure_changes.values()):
        return answer("held", ["The measured case held within its accepted limits."])
    return answer("not proven", ["There are no comparable measurements for this case."])


def build_verdict(case_verdicts, impact):
    answers = {case: verdict["answer"] for case, verdict in case_verdicts.items()}
    for word in ("receded", "failing"):
        cases = sorted(c for c, a in answers.items() if a == word)
        if cases:
            return {"answer": word, "because": [f"{word.capitalize()}: " + ", ".join(cases) + "."]}
    if impact is None:
        return {"answer": "not proven", "because": ["The scope of affected cases has not been established."]}
    if impact.get("unclassified"):
        return {"answer": "not proven", "because": ["Some changed files have unknown impact."]}
    scope = impact.get("cases", {})
    if answers.keys() - scope.keys():
        return {"answer": "not proven", "because": ["The impact assessment omits evaluated cases."]}
    pending = sorted(c for c, v in scope.items()
                     if v.get("answer") != "no_identified_dependency" and answers.get(c) not in {"held", "grew"})
    pending += sorted(c for c, a in answers.items() if c not in pending and a not in {"held", "grew"})
    if pending or not answers or not scope:
        detail = ", ".join(pending) or "the affected cases"
        return {"answer": "not proven", "because": [f"Gameplay evidence is still incomplete for {detail}."]}
    grew = sorted(c for c, a in answers.items() if a == "grew")
    if grew:
        return {"answer": "grew", "because": ["Improved cases: " + ", ".join(grew) + "."]}
    return {"answer": "held", "because": ["The evaluated cases held and the affected cases are covered."]}
