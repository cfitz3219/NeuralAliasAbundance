"""The checker is tested on intentionally wrong results before its green is
trusted (Astra's review, 2026-11-11): six that must be caught and two that must
not raise a false alarm. Synthetic logs only; no game is needed.

Until the judgment rules of rules/ exist, the cautious stand-ins answer. These
tests therefore assert facts (a floor broken, a value unknown, a case reached)
and that nothing wrong is ever reported as fine. The words for "held" and "grew"
belong to the rule vectors.
"""
import os
import tempfile
import unittest

import synthetic
import compare
import impact
import ingest
import report
import store

A, B = "a" * 40, "b" * 40
CASE = {"schema": "growth.case/1", "id": "demo-dx11", "game": "Demo game", "profile": "demo", "route": "B",
        "api": "dx11", "provider": "native11", "budget": {"fps_min": 40}}
CASE12 = dict(CASE, id="demo-dx12", game="Demo DX12 game", api="dx12", provider="dx12")
CASE_A = dict(CASE, id="demo-a", game="Demo native DLSS game", route="A", api="dx12", provider="route_a")
CASES = {c["id"]: c for c in (CASE, CASE12, CASE_A)}
DEPS = store.load(os.path.join(store.GROWTH, "dependencies.json"))


def run(log_text, case=CASE, stamp="20260101-000000", **kw):
    d = tempfile.mkdtemp(prefix=f"NA-demo-results-{stamp}-")
    with open(os.path.join(d, "status-1.txt"), "w", encoding="utf-8", newline="") as f:
        f.write(log_text)
    return ingest.ingest(d, case, **kw)[0]


def floor_from(result, entries):
    return {"schema": "growth.floor/1", "case": result["manifest"]["case"], "floor": entries,
            "set_by": {"evidence_id": result["manifest"]["evidence_id"], "build": result["manifest"]["producer"]["build"]},
            "proposal": None, "history": []}


class MustBeCaught(unittest.TestCase):
    def test_1_wrong_build_evidence(self):
        r = run(synthetic.steady(20, build=A), expect_build=B)
        self.assertEqual(r["status"], "refused")
        comparison = compare.compare_build(B, CASES, [r], {})
        self.assertIn("missing", comparison["cases"]["demo-dx11"])  # it cannot stand in for build B

    def test_2_missing_value_cannot_satisfy_a_maximum(self):
        base = run(synthetic.steady(20, build=A))
        # A log in which nothing counts deliveries: no gap can be measured.
        cand = run(synthetic.steady(20, build=B).replace("native11_dlaa delivered=", "native11_dlaa handed="), stamp="20260102-000000")
        self.assertEqual(cand["measurements"]["longest_gap_seconds"]["state"], "unknown")
        self.assertIsNone(cand["measurements"]["longest_gap_seconds"]["value"])  # not 0.0, which would pass any maximum
        floor = floor_from(base, {"longest_gap_seconds": {"kind": "max", "value": 8.0}})
        c = compare.compare_case(CASE, floor, base, cand)
        self.assertEqual(c["measures"]["longest_gap_seconds"]["against_floor"], "unknown")
        self.assertNotIn(c["verdict"]["answer"], ("held", "grew", "pass"))

    def test_3_counter_reset_is_not_subtracted_across(self):
        rows = [(i * 500, i * 500, i * 500) for i in range(6)] + [(i * 500, i * 500, 0) for i in range(6)]
        r = run(synthetic.native11(rows, build=A))
        self.assertEqual(len(r["sessions"]), 2)
        self.assertEqual(r["measurements"]["frames_presented"]["value"], 5000)
        self.assertEqual(r["measurements"]["frames_delivered"]["value"], 2500)  # not 0 - 2500, and not 0

    def test_4_recognising_fewer_scenes_cannot_look_better(self):
        base = run(synthetic.steady(40, scene_share=1.0, delivered_share=0.9, build=A))
        cand = run(synthetic.steady(40, scene_share=0.5, delivered_share=1.0, build=B), stamp="20260102-000000")
        # The old single number improves: 90% -> 100% of the scenes it recognised.
        self.assertGreater(cand["measurements"]["delivered_share_of_scene"]["value"], base["measurements"]["delivered_share_of_scene"]["value"])
        floor = floor_from(base, {"delivered_share_of_scene": {"kind": "min", "value": 90.0},
                                  "delivered_share_of_presented": {"kind": "min", "value": 85.0}})
        c = compare.compare_case(CASE, floor, base, cand)
        self.assertEqual(c["measures"]["delivered_share_of_scene"]["against_floor"], "holds")
        self.assertEqual(c["measures"]["delivered_share_of_presented"]["against_floor"], "below")
        self.assertLess(c["measures"]["scene_share_of_presented"]["change"], -40)
        self.assertEqual(c["verdict"]["answer"], "receded")

    def test_5_a_change_to_a_rule_that_lost_last_time_still_reaches_the_game(self):
        last = run(synthetic.steady(20, build=A))  # its camera was found by the inverse pair alone
        self.assertEqual(last["dependencies_in_use"], [])
        got = impact.impact(["src/providers/camera/relative_camera.hpp"], DEPS, CASES, {"demo-dx11": last})
        self.assertEqual(got["cases"]["demo-dx11"]["answer"], impact.AFFECTED)
        self.assertIn("the provider can select it", " ".join(got["cases"]["demo-dx11"]["because"]))
        self.assertEqual(got["cases"]["demo-a"]["answer"], impact.NONE_FOUND)

    def test_6_a_shared_dependency_reaches_both_providers(self):
        got = impact.impact(["src/host_dx12/dlss/backend.cpp"], DEPS, CASES)
        self.assertEqual({v["answer"] for v in got["cases"].values()}, {impact.AFFECTED})
        narrow = impact.impact(["src/host_dx12/native11/provider.cpp"], DEPS, CASES)
        self.assertEqual(narrow["cases"]["demo-dx11"]["answer"], impact.AFFECTED)
        self.assertEqual(narrow["cases"]["demo-dx12"]["answer"], impact.NONE_FOUND)

    def test_an_unclassified_change_is_unknown_never_empty(self):
        got = impact.impact(["somewhere/new/file.cpp", "docs/note.md"], DEPS, CASES)
        self.assertEqual({v["answer"] for v in got["cases"].values()}, {impact.UNKNOWN})
        self.assertEqual(impact.impact(["docs/note.md", "MASTER-PLAN.md"], DEPS, CASES)["cases"]["demo-dx11"]["answer"], impact.NONE_FOUND)

    def test_a_verified_job_that_now_fails_is_receding(self):
        base = run(synthetic.steady(20, build=A))
        cand = run(synthetic.steady(20, build=B).replace("viewport_jitter{route=1 on=1 frames=", "viewport_jitter{route=1 on=0 skipped="),
                   stamp="20260102-000000")
        cand2 = run("\r\n".join(line.split(" viewport_jitter")[0] + " viewport_jitter{route=1 on=0 frames=0 sets=0} | native11_dlaa" +
                                line.split("| native11_dlaa")[1] for line in synthetic.steady(20).splitlines() if line) + "\r\n",
                    stamp="20260103-000000")
        self.assertEqual(cand["jobs"]["jitter_applied"]["state"], "unknown")   # the counter is gone: unknown, not failed
        self.assertEqual(cand2["jobs"]["jitter_applied"]["state"], "failed")   # the counter says zero while DLAA ran
        self.assertEqual(compare.compare_case(CASE, None, base, cand2)["verdict"]["answer"], "receded")
        self.assertNotEqual(compare.compare_case(CASE, None, base, cand)["verdict"]["answer"], "receded")


class MustNotAlarm(unittest.TestCase):
    FLOOR = {"delivered_share_of_scene": {"kind": "min", "value": 90.0}, "delivered_share_of_presented": {"kind": "min", "value": 90.0},
             "fps_play_median": {"kind": "min", "value": 90.0}, "longest_gap_seconds": {"kind": "max", "value": 12.0}}

    def test_7_correctly_unchanged(self):
        base = run(synthetic.steady(40, delivered_share=0.95, build=A))
        cand = run(synthetic.steady(40, delivered_share=0.95, build=B), stamp="20260102-000000")
        c = compare.compare_case(CASE, floor_from(base, self.FLOOR), base, cand)
        self.assertEqual({m["against_floor"] for m in c["measures"].values() if "against_floor" in m}, {"holds"})
        self.assertNotEqual(c["verdict"]["answer"], "receded")
        self.assertEqual(c["guards"], [] if not compare.rule("guards")[1] else c["guards"])

    def test_8_correctly_improved_and_the_floor_does_not_move_by_itself(self):
        base = run(synthetic.steady(40, delivered_share=0.95, fps=95.0, build=A))
        cand = run(synthetic.steady(40, delivered_share=1.0, fps=110.0, build=B), stamp="20260102-000000")
        floor = floor_from(base, self.FLOOR)
        before = store.dump(floor)
        c = compare.compare_case(CASE, floor, base, cand)
        self.assertGreater(c["measures"]["delivered_share_of_scene"]["change"], 0)
        self.assertGreater(c["measures"]["fps_play_median"]["change"], 0)
        self.assertNotEqual(c["verdict"]["answer"], "receded")
        self.assertIn(c["floor_action"]["action"], ("keep", "propose"))  # never "confirm" from one better run
        self.assertEqual(store.dump(floor), before)


class WholeBuild(unittest.TestCase):
    def test_one_receding_case_is_never_hidden_by_the_others(self):
        base = run(synthetic.steady(40, build=A))
        good = run(synthetic.steady(40, build=B), stamp="20260102-000000")
        floors = {"demo-dx11": floor_from(base, {"delivered_share_of_presented": {"kind": "min", "value": 95.0}})}
        comparison = compare.compare_build(B, {"demo-dx11": CASE}, [base, good], floors)
        self.assertNotEqual(comparison["verdict"]["answer"], "receded")
        bad = run(synthetic.steady(40, scene_share=0.5, build=B), stamp="20260103-000000")
        comparison = compare.compare_build(B, {"demo-dx11": CASE}, [base, good, bad], floors)
        self.assertEqual(comparison["cases"]["demo-dx11"]["candidate"], bad["manifest"]["evidence_id"])  # the latest saved run speaks
        self.assertEqual(comparison["verdict"]["answer"], "receded")
        self.assertEqual(comparison["cases"]["demo-dx11"]["also_scored"], [good["manifest"]["evidence_id"]])

    def test_the_report_states_the_answer_and_escapes_what_it_prints(self):
        base = run(synthetic.steady(40, build=A))
        cand = run(synthetic.steady(40, scene_share=0.5, build=B), stamp="20260102-000000")
        evil = dict(CASE, game="<script>alert(1)</script>")
        floors = {"demo-dx11": floor_from(base, {"delivered_share_of_presented": {"kind": "min", "value": 95.0}})}
        comparison = compare.compare_build(B, {"demo-dx11": evil}, [base, cand], floors,
                                           ["src/host_dx12/native11/provider.cpp"], DEPS)
        html = report.page(comparison, {"demo-dx11": evil}, [base, cand], "2026-01-02")
        self.assertIn("receded", html)
        self.assertIn("below floor", html)
        self.assertNotIn("<script>alert", html)
        self.assertIn("&lt;script&gt;", html)
        row = report.ledger_row(comparison, {"demo-dx11": evil}, {r["manifest"]["evidence_id"]: r for r in (base, cand)}, "2026-01-02")
        self.assertEqual(row.count("|"), 9)


if __name__ == "__main__":
    unittest.main()
