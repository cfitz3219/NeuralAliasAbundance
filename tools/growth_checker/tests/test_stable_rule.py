"""Cort's rule (2026-11-12): 90% or more of game frames with DLAA is stable."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import scoring  # noqa: E402


def jobs():
    return {"continuity": {"state": "unknown"}, "dlaa_reaches_picture": {"state": "unknown"}}


class StableRule(unittest.TestCase):
    def test_at_or_above_ninety_is_stable(self):
        j = jobs()
        self.assertTrue(scoring.apply_stable_rule("B", j, 90.0))
        self.assertEqual(j["continuity"]["state"], "verified")
        self.assertEqual(j["dlaa_reaches_picture"]["state"], "verified")
        self.assertEqual(j["continuity"]["source"], "owner_verdict")

    def test_below_ninety_or_unmeasured_is_untouched(self):
        for share in (89.9, None):
            j = jobs()
            self.assertFalse(scoring.apply_stable_rule("B", j, share))
            self.assertEqual(j["continuity"]["state"], "unknown")

    def test_route_a_is_untouched(self):
        j = jobs()
        self.assertFalse(scoring.apply_stable_rule("A", j, 99.0))


if __name__ == "__main__":
    unittest.main()
