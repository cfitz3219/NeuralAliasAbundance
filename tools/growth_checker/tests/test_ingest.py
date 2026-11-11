import os
import tempfile
import unittest

import synthetic  # noqa: F401  (puts the tool on sys.path)
import ingest
import measure
import statuslog

BUILD_A = "a" * 40
BUILD_B = "b" * 40


def folder(files):
    d = tempfile.mkdtemp(prefix="NA-wwii-results-20260101-000000-")
    for name, text in files.items():
        with open(os.path.join(d, name), "w", encoding="utf-8", newline="") as f:
            f.write(text)
    return d


class Reading(unittest.TestCase):
    def test_counts_and_three_coverage_numbers(self):
        log = synthetic.steady(21, scene_share=0.8, delivered_share=0.5, build=BUILD_A)
        r = ingest.ingest(folder({"status-1.txt": log}))[0]
        self.assertEqual(r["status"], "scored")
        m = r["measurements"]
        self.assertEqual(m["frames_presented"]["value"], 10000)
        self.assertEqual(m["frames_scene"]["value"], 8000)
        self.assertEqual(m["frames_delivered"]["value"], 4000)
        self.assertEqual(m["scene_share_of_presented"]["value"], 80.0)
        self.assertEqual(m["delivered_share_of_scene"]["value"], 50.0)
        self.assertEqual(m["delivered_share_of_presented"]["value"], 40.0)
        self.assertEqual(r["manifest"]["producer"], {"build": BUILD_A, "build_source": "status_header"})

    def test_missing_value_is_unknown_never_zero(self):
        r = ingest.ingest(folder({"status-1.txt": synthetic.steady(5, jitter=False)}))[0]
        jitter = r["measurements"]["frames_jittered"]
        self.assertEqual(jitter["state"], "unknown")
        self.assertIsNone(jitter["value"])
        self.assertIsNone(measure.value(r["measurements"], "frames_jittered"))
        self.assertEqual(r["jobs"]["jitter_applied"]["state"], "unknown")

    def test_dx12_between_list_route_that_delivers_nothing_is_a_failure(self):
        # The shape of the first real between-list run: scene frames seen, no camera, nothing delivered,
        # and an inline coverage line that stays at zero. It must not read as "no opportunities".
        rows = [{"presented": i * 800, "scene": i * 300, "delivered": 0, "no_camera": i * 300, "lost_no_vp": i * 300,
                 "menu": i * 10, "sharpness": 49 if i < 3 else 100} for i in range(8)]
        r = ingest.ingest(folder({"status-1.log": synthetic.dx12(rows, build=BUILD_A, route="between")}))[0]
        m, jobs = r["measurements"], r["jobs"]
        self.assertEqual((m["frames_presented"]["value"], m["frames_scene"]["value"], m["frames_delivered"]["value"]), (5600, 2100, 0))
        self.assertEqual(jobs["dlaa_delivered"]["state"], "failed")
        self.assertEqual(jobs["camera_found"]["state"], "failed")
        self.assertEqual(m["missed_because"]["value"], {"no_camera": 2100})
        self.assertEqual(m["camera_scan_losses"]["value"], {"lost_no_vp": 2100})
        # Fps without DLSS running does not test the budget, and a slider moved while nothing ran proves nothing.
        self.assertEqual(jobs["performance_budget"]["state"], "untested")
        self.assertEqual(jobs["home_controls_take_effect"]["state"], "untested")

    def test_dx12_counts_both_routes_and_keeps_scan_losses_apart_from_misses(self):
        # Inline frames carry their own jitter count; between-list frames declare a viewport sample.
        rows = [{"presented": i * 1000, "scene": i * 950, "delivered": i * 940, "inline": i * 900, "jittered_inline": i * 899,
                 "no_camera": i, "lost_no_vp": i * 200, "held": i * 150} for i in range(11)]
        r = ingest.ingest(folder({"status-1.log": synthetic.dx12(rows, build=BUILD_A)}))[0]
        m = r["measurements"]
        self.assertEqual(m["frames_delivered"]["value"], 9400)       # the route's count, not the inline coverage line (9000)
        self.assertEqual(m["delivered_share_of_scene"]["value"], 98.95)
        self.assertEqual(m["frames_jittered"]["value"], 8990 + 400)  # inline with a sample + between-list declared
        self.assertEqual(m["missed_because"]["value"], {"no_camera": 10})
        self.assertEqual(m["camera_scan_losses"]["value"], {"lost_no_vp": 2000})  # scans, mostly rescued: not missed frames
        self.assertEqual(m["frames_on_predicted_camera"]["value"], 1500)
        self.assertEqual(r["jobs"]["camera_found"]["state"], "verified")
        self.assertEqual({k: r["patterns"][k].get("value") for k in ("scene_placement", "jitter_route")},
                         {"scene_placement": "inline", "jitter_route": "viewport"})
        self.assertEqual(r["jobs"]["home_controls_take_effect"]["state"], "untested")  # a pattern is not a setting

    def test_dx12_matrix_jitter_is_not_read_as_no_jitter(self):
        rows = [{"presented": i * 1000, "scene": i * 1000, "delivered": i * 1000, "declared": 0} for i in range(6)]
        r = ingest.ingest(folder({"status-1.log": synthetic.dx12(rows, route="between", jitter_mode=0)}))[0]
        self.assertEqual(r["measurements"]["frames_jittered"]["state"], "unknown")
        self.assertEqual(r["jobs"]["jitter_applied"]["state"], "unknown")   # not "failed": the log cannot say

    def test_a_launcher_log_with_no_sample_is_empty_not_unreadable(self):
        d = folder({"status-13084.log": f"NeuralAlias build {BUILD_A}\r\n", "status-43256.log": synthetic.steady(6, build=BUILD_A)})
        got = ingest.ingest(d, expect_build=BUILD_A)
        self.assertEqual([r["status"] for r in got], ["empty", "scored"])   # both recorded, neither dropped
        self.assertNotIn("measurements", got[0])

    def test_extracts_beside_a_log_are_not_runs(self):
        log = synthetic.steady(6, build=BUILD_A)
        d = folder({"status-48964.log": log, "status-48964-j2_camera.txt": "j2_camera contested=0\r\n", "status-777.log": log})
        got = ingest.ingest(d)
        self.assertEqual([r["manifest"]["session"]["id"] for r in got], ["48964", "777"])
        roles = {f["name"]: f["role"] for f in got[0]["manifest"]["files"]}
        self.assertEqual(roles["status-48964-j2_camera.txt"], "extract")

    def test_counter_reset_splits_sessions(self):
        first = [(i * 500, i * 500, i * 500) for i in range(5)]    # 2000 frames, then the game restarts
        second = [(i * 500, i * 500, i * 500) for i in range(3)]   # 1000 more
        r = ingest.ingest(folder({"status-1.txt": synthetic.native11(first + second)}))[0]
        self.assertEqual(len(r["sessions"]), 2)
        self.assertEqual(r["measurements"]["frames_presented"]["value"], 3000)  # not 1000 - 0, not negative
        self.assertEqual([s["presented"] for s in r["sessions"]], [2000, 1000])

    def test_bad_stretch_stays_visible_beside_conditional_fps(self):
        rows = [{"presented": i * 500, "scene": i * 500, "delivered": i * 500, "fps": 100.0} for i in range(11)]
        for k in range(1, 11):  # then ten slow samples with nothing delivered
            rows.append({"presented": 5000 + k * 100, "scene": 5000 + k * 100, "delivered": 5000, "fps": 20.0})
        r = ingest.ingest(folder({"status-1.txt": synthetic.native11(rows)}))[0]
        m = r["measurements"]
        self.assertEqual(m["fps_median_while_delivering"]["value"], 100.0)
        self.assertEqual(m["fps_play_lowest"]["value"], 20.0)
        self.assertEqual(m["not_delivering_share_of_play"]["value"], 50.0)
        self.assertEqual(m["longest_gap_seconds"]["value"], 50.0)

    def test_settings_before_delivery_are_not_changes(self):
        rows = [{"presented": 0, "scene": 0, "delivered": 0, "sharpness": 0}] + \
               [{"presented": i * 500, "scene": i * 500, "delivered": i * 500, "sharpness": 50} for i in range(1, 6)]
        r = ingest.ingest(folder({"status-1.txt": synthetic.native11(rows)}))[0]
        self.assertEqual(r["observed"]["sharpness"]["distinct"], [50])
        self.assertEqual(r["jobs"]["home_controls_take_effect"]["state"], "untested")


class Provenance(unittest.TestCase):
    def test_wrong_build_is_refused(self):
        d = folder({"status-1.txt": synthetic.steady(5, build=BUILD_A)})
        r = ingest.ingest(d, expect_build=BUILD_B[:8])[0]
        self.assertEqual(r["status"], "refused")
        self.assertNotIn("measurements", r)
        self.assertEqual(r["manifest"]["producer"]["build"], BUILD_A)  # the label cannot rename the evidence

    def test_no_build_cannot_stand_for_an_expected_build(self):
        r = ingest.ingest(folder({"status-1.txt": synthetic.steady(5)}), expect_build=BUILD_A)[0]
        self.assertEqual(r["status"], "refused")

    def test_log_header_outranks_the_saved_summary(self):
        d = folder({"status-1.txt": synthetic.steady(5, build=BUILD_A), "summary.txt": f"Results\nBuild: {BUILD_B}\n"})
        self.assertEqual(ingest.ingest(d)[0]["manifest"]["producer"], {"build": BUILD_A, "build_source": "status_header"})
        d = folder({"status-1.txt": synthetic.steady(5), "summary.txt": f"Results\nBuild: {BUILD_B}\n"})
        man = ingest.ingest(d)[0]["manifest"]
        self.assertEqual(man["producer"], {"build": BUILD_B, "build_source": "results_summary"})
        self.assertTrue(man["notes"])

    def test_mixed_builds_in_one_log_have_no_build(self):
        text = synthetic.steady(3, build=BUILD_A) + synthetic.steady(3, build=BUILD_B)
        man = ingest.ingest(folder({"status-1.txt": text}))[0]["manifest"]
        self.assertIsNone(man["producer"]["build"])
        self.assertEqual(man["producer"]["build_source"], "unknown")

    def test_every_log_in_a_folder_is_its_own_evidence(self):
        d = folder({"status-1.txt": synthetic.steady(40, build=BUILD_A), "status-2.txt": synthetic.steady(4, build=BUILD_A)})
        records = ingest.ingest(d)
        self.assertEqual(len(records), 2)  # the small one is not dropped for being small
        self.assertEqual(len({r["manifest"]["evidence_id"] for r in records}), 2)

    def test_same_evidence_keeps_its_id(self):
        d = folder({"status-1.txt": synthetic.steady(5, build=BUILD_A)})
        self.assertEqual(ingest.ingest(d)[0]["manifest"]["evidence_id"], ingest.ingest(d)[0]["manifest"]["evidence_id"])


class Unreadable(unittest.TestCase):
    def test_a_log_without_samples_is_unreadable_not_a_result(self):
        r = ingest.ingest(folder({"status-1.txt": "nothing useful here\n"}))[0]
        self.assertEqual(r["status"], "unreadable")
        self.assertNotIn("jobs", r)

    def test_an_empty_folder_is_unreadable(self):
        self.assertEqual(ingest.ingest(folder({"readme.txt": "x"}))[0]["status"], "unreadable")

    def test_unlogged_time_is_not_play(self):
        rows = synthetic.native11([(i * 500, i * 500, i * 500) for i in range(5)])
        late = synthetic.native11([(2500 + i * 500, 2500 + i * 500, 2500 + i * 500) for i in range(5)], start=13 * 3600)
        m = ingest.ingest(folder({"status-1.txt": rows + late}))[0]["measurements"]
        self.assertGreater(m["unlogged_seconds"]["value"], 3000)
        self.assertEqual(m["play_seconds"]["value"], 40.0)


class Events(unittest.TestCase):
    def test_events_are_read_and_kept_out_of_samples(self):
        text = synthetic.steady(2) + "event t=10 frame=7 thread=1 src=code native11_relative learned frames=500\r\n" + synthetic.steady(1, start=13 * 3600)
        parsed = statuslog.parse(text)
        self.assertEqual(parsed["events"][0]["name"], "native11_relative")
        self.assertEqual(parsed["events"][0]["frame"], 7)
        self.assertFalse(any("event t=" in s["text"] for s in parsed["samples"]))


if __name__ == "__main__":
    unittest.main()
