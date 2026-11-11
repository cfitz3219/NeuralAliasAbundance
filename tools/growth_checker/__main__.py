"""python3 tools/growth_checker <command> ...

  ingest <saved run folder or status log> [--case ID] [--type live_run|replay|fixture_test|synthetic]
         [--expect-build HASH] [--verdict JOB=STATE:"what the owner said"] [--save] [--json]
  selftest

Exit codes of ingest: 0 scored, 2 evidence unreadable, 3 evidence refused.
(A regression is reported by `compare`, never by these codes.)
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
    for r in records:
        worst = max(worst, {"scored": 0, "unreadable": 2, "refused": 3}[r["status"]])
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


def main(argv):
    if not argv:
        sys.exit(__doc__)
    if argv[0] == "ingest":
        return cmd_ingest(argv[1:])
    if argv[0] == "selftest":
        import unittest
        suite = unittest.defaultTestLoader.discover(os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
        return 0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
