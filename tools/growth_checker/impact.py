"""Which cases a source change can reach, from growth/dependencies.json.

Three answers per case:
  affected            a changed file belongs to an area that reaches the case's provider
  no_identified_dependency
                      every changed file is classified and none reaches the case.
                      This rests on regression tests; it does not prove the game untouched.
  impact_unknown      a changed file matches no area

Eligibility counts, not the last winner: a rule a game rejected last time can
start winning after a change to it. The list spends play time well; it does not
renew any game's earlier live verification.
"""
import fnmatch

AFFECTED, NONE_FOUND, UNKNOWN = "affected", "no_identified_dependency", "impact_unknown"


def classify(path, areas):
    """The area with the longest matching path, or None."""
    best = None
    for area in areas:
        for pattern in area["paths"]:
            if "*" in pattern:
                hit = "/" not in path and fnmatch.fnmatch(path, pattern)
                weight = 1
            else:
                hit = path == pattern or path.startswith(pattern)
                weight = len(pattern)
            if hit and (best is None or weight > best[0]):
                best = (weight, area)
    return best[1] if best else None


def impact(changed_files, dependencies, cases, last_results=None):
    """-> {"files": {path: area id or None}, "unclassified": [...],
           "cases": {case id: {"answer", "because": [sentences], "areas": [ids]}}}

    last_results: {case id: result}; used only to say whether a touched variant was
    in use in the case's last run. It never removes a case from the list.
    """
    areas = dependencies["areas"]
    files, unclassified, touched = {}, [], {}
    for path in sorted(set(changed_files)):
        area = classify(path, areas)
        files[path] = area["id"] if area else None
        if area is None:
            unclassified.append(path)
        else:
            touched.setdefault(area["id"], area)
    out = {}
    for case_id, case in sorted(cases.items()):
        because, hit = [], []
        for area in touched.values():
            if case["provider"] in area["reaches"]:
                hit.append(area["id"])
                line = f"{area['id']}: {area['why']}"
                used = {d["variant"] for d in ((last_results or {}).get(case_id) or {}).get("dependencies_in_use", [])}
                variants = set(area.get("variants", []))
                if variants:
                    line += (" In use in this case's last run: " + ", ".join(sorted(variants & used)) + "."
                             if variants & used else " Not the rule that won in this case's last run, but the provider can select it.")
                because.append(line)
        if hit:
            answer = AFFECTED
        elif unclassified:
            answer = UNKNOWN
            because.append("Not classified in growth/dependencies.json: " + ", ".join(unclassified))
        else:
            answer = NONE_FOUND
            because.append("Every changed file is classified and none reaches this case's provider. "
                           "That rests on the regression tests; it is not proof the game is untouched.")
        out[case_id] = {"answer": answer, "because": because, "areas": hit}
    return {"files": files, "unclassified": unclassified, "cases": out}
