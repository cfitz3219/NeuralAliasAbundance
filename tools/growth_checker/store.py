"""Where cases, results and floors live (the growth/ folder of the repository).

Raw logs are never stored here. A result names its log files by hash.
One result file per piece of evidence and scorer version: a newer scorer writes
a new file beside the old one, so an earlier analysis stays readable.
"""
import json
import os

from version import VERSION

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GROWTH = os.path.join(ROOT, "growth")


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def dump(obj):
    return json.dumps(obj, indent=2, sort_keys=False, ensure_ascii=False) + "\n"


def save(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(dump(obj))


def cases(growth=GROWTH):
    folder = os.path.join(growth, "cases")
    out = {}
    if os.path.isdir(folder):
        for name in sorted(os.listdir(folder)):
            if name.endswith(".json"):
                c = load(os.path.join(folder, name))
                out[c["id"]] = c
    return out


def result_path(result, growth=GROWTH):
    m = result["manifest"]
    build = (m["producer"]["build"] or "nobuild")[:8]
    return os.path.join(growth, "results", build, m["case"] or "unassigned", f"{m['evidence_id']}@{VERSION}.json")


def results(growth=GROWTH):
    """Every stored result; where one piece of evidence was scored by several
    scorer versions, only the newest analysis is returned."""
    newest = {}
    folder = os.path.join(growth, "results")
    for base, _, names in os.walk(folder):
        for name in names:
            if name.endswith(".json"):
                r = load(os.path.join(base, name))
                key = (r["manifest"]["evidence_id"], r["manifest"]["case"])
                rank = tuple(int(x) for x in r["manifest"]["scorer"]["version"].split("."))
                if key not in newest or rank > newest[key][0]:
                    newest[key] = (rank, r)
    return [r for _, r in newest.values()]


def floor(case_id, growth=GROWTH):
    path = os.path.join(growth, "floors", case_id + ".json")
    return load(path) if os.path.isfile(path) else None
