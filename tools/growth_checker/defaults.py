"""Cautious stand-ins for the judgment rules, used until a module of the same
name exists in rules/ (tasks A1 to A4). They never claim growth: a comparison is
"not judged", no floor moves, and the answer is "receded" on a plain fact (a
floor broken, a verified job now failing) and otherwise "not proven".
"""


class comparison:
    @staticmethod
    def judge(baseline, candidate, case):
        return {"verdict": "not_judged", "limits": [], "tolerance": {},
                "reasons": ["The rule for valid comparisons (rules/comparison.py) is not in this build of the checker."]}

    @staticmethod
    def floor_action(case, floor, candidate, comparison, earlier_proposal):
        return {"action": "keep", "floor": None, "reason": "No floor moves until the comparison rule exists."}


class guards:
    @staticmethod
    def check(baseline, candidate, case):
        return []


class transfer:
    @staticmethod
    def credit(results_by_case, cases):
        return {"credits": [], "combinations": [], "title_specific_in_use": [], "not_judged": True}


class ambiguity:
    @staticmethod
    def job_state(pieces):
        states = {p["state"] for p in pieces}
        if len(states) == 1:
            return {"state": states.pop(), "because": "All evidence agrees."}
        return {"state": "unknown", "because": "The evidence disagrees and the rule for that (rules/ambiguity.py) is not in this build."}

    @staticmethod
    def case_verdict(comparison, measure_changes, jobs_before, jobs_after, guards):
        because = [f"{name} is {c['candidate']} against a floor of {c['floor']['kind']} {c['floor']['value']}."
                   for name, c in measure_changes.items() if c.get("against_floor") == "below"]
        because += [f"{job} was verified and now fails." for job, before in (jobs_before or {}).items()
                    if before["state"] == "verified" and (jobs_after or {}).get(job, {}).get("state") == "failed"]
        because += [g["message"] for g in guards if g.get("severity") == "blocks"]
        if because:
            return {"answer": "receded", "because": because}
        return {"answer": "not proven", "because": ["No floor is broken and no verified job fails. Whether the build held or grew "
                                                    "is not judged until the comparison and ambiguity rules exist."]}

    @staticmethod
    def build_verdict(case_verdicts, impact):
        receded = sorted(c for c, v in case_verdicts.items() if v["answer"] == "receded")
        if receded:
            return {"answer": "receded", "because": ["Receded: " + ", ".join(receded) + "."]}
        return {"answer": "not proven", "because": ["No case receded. Nothing more is claimed until the judgment rules exist."]}
