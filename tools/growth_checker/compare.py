"""A build against its baseline, case by case.

Facts are computed here: each measurement beside its floor, each job before and
after. Judgments come from rules/ (Astra's tasks): whether two results may be
compared at all, which results mislead, what the evidence adds up to.
"""
import importlib
import os
import sys

import defaults
from measure import value

HERE = os.path.dirname(os.path.abspath(__file__))


def rule(name):
    """rules/<name>.py when it exists, else the cautious stand-in."""
    if os.path.isfile(os.path.join(HERE, "rules", name + ".py")):
        if HERE not in sys.path:
            sys.path.insert(0, HERE)
        return importlib.import_module("rules." + name), True
    return getattr(defaults, name), False


def against_floor(floor, result, tolerance=None):
    """-> {measurement: {"floor", "candidate", "against_floor": holds | below | unknown}}

    A measurement the result does not have is "unknown". Unknown never holds a
    floor: a missing longest gap cannot satisfy a maximum.
    """
    out = {}
    for name, f in ((floor or {}).get("floor") or {}).items():
        got = value(result.get("measurements", {}), name)
        slack = 0.0
        tol = (tolerance or {}).get(name) or {}
        if got is not None and isinstance(got, (int, float)):
            slack = max(tol.get("absolute", 0.0), abs(f["value"]) * tol.get("relative_percent", 0.0) / 100.0)
        if got is None or not isinstance(got, (int, float)):
            state = "unknown"
        elif f["kind"] == "min":
            state = "holds" if got >= f["value"] - slack else "below"
        else:
            state = "holds" if got <= f["value"] + slack else "below"
        out[name] = {"floor": f, "candidate": got, "against_floor": state}
        if state == "unknown":
            out[name]["why"] = result.get("measurements", {}).get(name, {}).get("why", "the result has no such measurement")
    return out


def changes(baseline, candidate, floor_checks):
    """Every numeric measurement of the candidate beside the baseline's, plus the floor checks."""
    out = {}
    for name, m in candidate.get("measurements", {}).items():
        new = m["value"] if m["state"] == "measured" else None
        old = value(baseline.get("measurements", {}), name) if baseline else None
        if not isinstance(new, (int, float)) and not isinstance(old, (int, float)):
            continue
        row = {"baseline": old, "candidate": new, "unit": m.get("unit")}
        if isinstance(new, (int, float)) and isinstance(old, (int, float)):
            row["change"] = round(new - old, 3)
        out[name] = row
    for name, check in floor_checks.items():
        out.setdefault(name, {"baseline": value(baseline.get("measurements", {}), name) if baseline else None,
                              "candidate": check["candidate"]}).update(check)
    return out


def compare_case(case, floor, baseline, candidate):
    """-> the comparison record of one case (see schema.md)."""
    comparison_rule, _ = rule("comparison")
    guard_rule, _ = rule("guards")
    ambiguity_rule, _ = rule("ambiguity")
    comp = (comparison_rule.judge(baseline, candidate, case) if baseline else
            {"verdict": "no_baseline", "limits": [], "reasons": ["This case has no earlier result to compare with."], "tolerance": {}})
    checks = against_floor(floor, candidate, comp.get("tolerance"))
    measure_changes = changes(baseline, candidate, checks)
    flagged = guard_rule.check(baseline, candidate, case)
    verdict = ambiguity_rule.case_verdict(comp, measure_changes, (baseline or {}).get("jobs"), candidate.get("jobs"), flagged)
    proposal = comparison_rule.floor_action(case, floor, candidate, comp, (floor or {}).get("proposal")) if baseline or floor else None
    return {
        "case": case["id"],
        "baseline": baseline["manifest"]["evidence_id"] if baseline else None,
        "candidate": candidate["manifest"]["evidence_id"],
        "comparison": comp,
        "measures": measure_changes,
        "jobs": {job: {"before": ((baseline or {}).get("jobs") or {}).get(job, {}).get("state"), "after": j["state"]}
                 for job, j in candidate.get("jobs", {}).items()},
        "guards": flagged,
        "floor_action": proposal,
        "verdict": verdict,
    }


def pick(results, case_id, build=None, not_build=None):
    """The stored results of a case for one build (or for any build but one), oldest saved first."""
    out = [r for r in results if r["manifest"]["case"] == case_id and r["status"] == "scored"]
    b = lambda r: r["manifest"]["producer"]["build"] or ""
    if build:
        out = [r for r in out if b(r).startswith(build.lower()) or build.lower().startswith(b(r) or "?")]
    if not_build:
        out = [r for r in out if not (b(r).startswith(not_build.lower()) or not_build.lower().startswith(b(r) or "?"))]
    return sorted(out, key=lambda r: (r["manifest"]["session"]["saved_at"] or "", r["manifest"]["evidence_id"]))


def compare_build(build, cases, results, floors, changed_files=None, dependencies=None):
    """-> {"build", "cases": {id: comparison or {"missing": reason}}, "impact", "verdict", "rules_in_place": {...}}"""
    import impact as impacts
    out = {"build": build, "cases": {}, "impact": None,
           "rules_in_place": {name: rule(name)[1] for name in ("comparison", "guards", "transfer", "ambiguity")}}
    last = {}
    for case_id, case in sorted(cases.items()):
        mine = pick(results, case_id, build=build)
        floor = floors.get(case_id)
        baseline = None
        if floor and floor.get("set_by"):
            baseline = next((r for r in results if r["manifest"]["evidence_id"] == floor["set_by"]["evidence_id"]), None)
        same = lambda r: mine and r["manifest"]["evidence_id"] == mine[-1]["manifest"]["evidence_id"]
        if baseline is None or same(baseline):
            # The run that set the floor is not compared with itself (Astra, A1 compile note): take a
            # distinct earlier result when there is one, else show the floor with no comparison.
            earlier = [r for r in pick(results, case_id, not_build=build) if not same(r)]
            baseline = earlier[-1] if earlier else None
        if baseline:
            last[case_id] = baseline
        if not mine:
            out["cases"][case_id] = {"missing": f"No scored evidence of build {build} for this case.",
                                     "baseline": baseline["manifest"]["evidence_id"] if baseline else None}
            continue
        out["cases"][case_id] = compare_case(case, floor, baseline, mine[-1])
        out["cases"][case_id]["also_scored"] = [r["manifest"]["evidence_id"] for r in mine[:-1]]
    if changed_files is not None and dependencies:
        out["impact"] = impacts.impact(changed_files, dependencies, cases, last)
    ambiguity_rule, _ = rule("ambiguity")
    verdicts = {c: v["verdict"] for c, v in out["cases"].items() if "verdict" in v}
    out["verdict"] = ambiguity_rule.build_verdict(verdicts, out["impact"])
    return out
