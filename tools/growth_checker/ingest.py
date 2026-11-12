"""A saved run becomes result records: one per status log in the folder."""
import manifest as manifests
import measure as measures
import patterns
import scoring
import statuslog
from version import SCHEMA

# The outcome of reading evidence is kept apart from any verdict about the build.
# "empty" is a status log with no sample in it: a process that started and left
# (a launcher) writes one. It is recorded, and is not a failure to read evidence.
SCORED, UNREADABLE, REFUSED, EMPTY = "scored", "unreadable", "refused", "empty"


def ingest(path, case=None, evidence_type="live_run", expect_build=None, owner_verdicts=None):
    """-> list of result records. `case` is a case record (growth/cases) or None.

    expect_build is a check, not a label: evidence from another build, or with no
    build in it, is refused. It can never rename what the evidence says it is.
    """
    logs = manifests.status_logs(path)
    if not logs:
        return [{"schema": f"growth.result/{SCHEMA}", "manifest": None, "status": UNREADABLE,
                 "status_reason": f"no status log in {path}"}]
    out = []
    for log in logs:
        parsed = statuslog.parse(manifests.read_text(log))
        man = manifests.make(log, parsed, evidence_type, case["id"] if case else None, "ingest argument" if case else None)
        result = {"schema": f"growth.result/{SCHEMA}", "manifest": man, "status": SCORED, "status_reason": None}
        build = man["producer"]["build"]
        if expect_build and not manifests.build_matches(build, expect_build):
            result["status"] = REFUSED
            result["status_reason"] = (f"the evidence is from build {build}, not the expected {expect_build}" if build
                                       else f"the evidence carries no build line, so it cannot stand for {expect_build}")
            out.append(result)
            continue
        if not parsed["samples"] and (parsed["builds"] or parsed["events"]):
            result["status"] = EMPTY
            result["status_reason"] = "the log holds a header and no status sample (the process started and wrote nothing more)"
            out.append(result)
            continue
        got = measures.measure(parsed, case.get("provider") if case else None)
        if "error" in got:
            result["status"] = UNREADABLE
            result["status_reason"] = got["error"]
            out.append(result)
            continue
        if case and got["detected_provider"] and got["detected_provider"] != got["provider"]:
            man["notes"].append(f"the case names provider {got['provider']}, but the log's counters look like {got['detected_provider']}")
        hint = man["session"]["profile_hint"]
        if case and hint and case.get("profile") and hint != case["profile"]:
            man["notes"].append(f"the saved folder is named for profile '{hint}', the case is for '{case['profile']}'")
        pats, deps = patterns.describe(got["provider"], got["api"], got["measurements"], got["observed"])
        result.update({
            "provider": got["provider"], "api": got["api"], "route": got["route"],
            "sessions": got["sessions"],
            "measurements": got["measurements"],
            "observed": got["observed"],
            "jobs": scoring.score(got["route"], got["measurements"], got["observed"], case, owner_verdicts),
            "owner_verdicts": list(owner_verdicts or []),
            "patterns": pats,
            "dependencies_in_use": deps,
        })
        out.append(result)
    return out
