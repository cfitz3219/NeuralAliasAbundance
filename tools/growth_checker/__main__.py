"""python3 tools/growth_checker <command> ...

  ingest <saved run folder or status log> [--case ID] [--type live_run|replay|fixture_test|synthetic]
         [--expect-build HASH] [--verdict JOB=STATE:"what the owner said"] [--save] [--json]
  compare --build HASH [--changed FROM..TO | --files a b c] [--json]
  report  --build HASH [--changed FROM..TO | --files a b c] [--out page.html] [--date YYYY-MM-DD]
  impact  FROM..TO | --files a b c
  selftest

Exit codes. ingest: 0 scored, 2 evidence unreadable (or nothing but empty logs), 3 evidence refused.
compare: 0 no case receded or failing, 1 a case receded or is failing, 2 no scored evidence for the build.
The two are kept apart on purpose: unreadable evidence is never a result.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ingest as ingestion  # noqa: E402
import store  # noqa: E402


def _option(argv, name, default=None, many=False):
    values = [argv[i + 1] for i, a in enumerate(argv[:-1]) if a == name]
    return values if many else (values[-1] if values else default)


def cmd_ingest(argv):
    if not argv or argv[0].startswith("--"):
        sys.exit(__doc__)
    case_id = _option(argv, "--case")
    cases = store.cases()
    if case_id and case_id not in cases:
        sys.exit(f"unknown case '{case_id}'; known: {', '.join(sorted(cases)) or 'none'}")
    verdicts = []
    for text in _option(argv, "--verdict", many=True):
        job, _, rest = text.partition("=")
        state, _, said = rest.partition(":")
        verdicts.append({"job": job, "state": state, "by": "Cort", "said": said})
    records = ingestion.ingest(argv[0], cases.get(case_id), _option(argv, "--type", "live_run"),
                               _option(argv, "--expect-build"), verdicts)
    worst = 0
    any_scored = any(r["status"] == "scored" for r in records)
    for r in records:
        # An empty log beside a scored one is a process that left early, not unread evidence.
        worst = max(worst, {"scored": 0, "unreadable": 2, "refused": 3, "empty": 0 if any_scored else 2}[r["status"]])
        if "--save" in argv and r["status"] == "scored":
            path = store.result_path(r)
            store.save(path, r)
            print("saved " + os.path.relpath(path, store.ROOT))
        if "--json" in argv:
            print(store.dump(r), end="")
            continue
        man = r["manifest"]
        print(f"== {man['files'][0]['name'] if man else argv[0]}: {r['status']}" + (f" ({r['status_reason']})" if r["status_reason"] else ""))
        if man:
            print(f"   build {man['producer']['build']} (from {man['producer']['build_source']}), evidence {man['evidence_id']}")
            for note in man["notes"]:
                print("   note: " + note)
        if r["status"] != "scored":
            continue
        print(f"   provider {r['provider']} ({r['api']}, route {r['route']}), sessions {len(r['sessions'])}")
        for name, m in r["measurements"].items():
            shown = m["value"] if m["state"] == "measured" else f"{m['state']}: {m.get('why', '')}"
            print(f"   {name:32s} {shown}")
        for job, j in r["jobs"].items():
            print(f"   [{j['state']:>14s}] {job}: {j['note']}")
    return worst


def _changed(argv):
    """The changed files named on the command line, or None when none were named."""
    if "--files" in argv:
        files = []
        for a in argv[argv.index("--files") + 1:]:
            if a.startswith("--"):  # the next option ends the list
                break
            files.append(a)
        return files
    span = _option(argv, "--changed") or (argv[0] if argv and ".." in argv[0] else None)
    if not span:
        return None
    import subprocess
    done = subprocess.run(["git", "-C", store.ROOT, "diff", "--name-only", span], capture_output=True, text=True)
    if done.returncode != 0:
        sys.exit("git could not list the changed files: " + done.stderr.strip())
    return [line for line in done.stdout.splitlines() if line]


def _compare(argv):
    import compare as comparing
    build = _option(argv, "--build")
    if not build:
        sys.exit(__doc__)
    cases = store.cases()
    floors = {c: store.floor(c) for c in cases}
    deps = store.load(os.path.join(store.GROWTH, "dependencies.json"))
    results = store.results()
    return comparing.compare_build(build, cases, results, floors, _changed(argv), deps), cases, results


def cmd_compare(argv):
    comparison, cases, _ = _compare(argv)
    if "--json" in argv:
        print(store.dump(comparison), end="")
    else:
        print(f"build {comparison['build']}: {comparison['verdict']['answer']}")
        for line in comparison["verdict"]["because"]:
            print("  " + line)
        for cid, c in comparison["cases"].items():
            if "missing" in c:
                print(f"  {cid}: {c['missing']}")
                continue
            print(f"  {cid}: {c['verdict']['answer']}")
            for name, row in c["measures"].items():
                if row.get("against_floor") in ("below", "unknown"):
                    print(f"      {name}: {row['candidate']} against floor {row['floor']['kind']} {row['floor']['value']} -> {row['against_floor']}")
            for g in c["guards"]:
                print(f"      {g['severity']}: {g['message']}")
    answers = [c["verdict"]["answer"] for c in comparison["cases"].values() if "verdict" in c]
    if not answers:
        return 2
    return 1 if any("reced" in a.lower() or "fail" in a.lower() for a in answers) else 0


def cmd_report(argv):
    import datetime
    import report
    comparison, cases, results = _compare(argv)
    date = _option(argv, "--date") or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    html = report.page(comparison, cases, results, date)
    out = _option(argv, "--out")
    if out:
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(html)
        print("wrote " + out)
    else:
        print(html)
    return 0


def cmd_impact(argv):
    import impact
    files = _changed(argv)
    if files is None:
        sys.exit(__doc__)
    deps = store.load(os.path.join(store.GROWTH, "dependencies.json"))
    result = impact.impact(files, deps, store.cases())
    for cid, v in result["cases"].items():
        print(f"{v['answer']:26s} {cid}")
        for line in v["because"]:
            print("    " + line)
    return 0


def main(argv):
    if not argv:
        sys.exit(__doc__)
    commands = {"ingest": cmd_ingest, "compare": cmd_compare, "report": cmd_report, "impact": cmd_impact}
    if argv[0] in commands:
        return commands[argv[0]](argv[1:])
    if argv[0] == "selftest":
        import unittest
        suite = unittest.defaultTestLoader.discover(os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
        return 0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
