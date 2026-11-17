"""Astra's rule vectors (tests/vectors/<rule>/*.json): each names a rule
function, its positional arguments and the exact expected answer."""
import glob
import importlib
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if HERE not in sys.path:
    sys.path.insert(0, HERE)


class RuleVectors(unittest.TestCase):
    def test_every_vector(self):
        files = sorted(glob.glob(os.path.join(HERE, "tests", "vectors", "*", "*.json")))
        self.assertTrue(files, "no rule vectors found")
        for path in files:
            rule = os.path.basename(os.path.dirname(path))
            with open(path, encoding="utf-8") as f:
                v = json.load(f)
            with self.subTest(vector=f"{rule}/{os.path.basename(path)}", about=v.get("about")):
                module = importlib.import_module("rules." + rule)
                self.assertEqual(getattr(module, v["call"])(*v["args"]), v["expect"])


if __name__ == "__main__":
    unittest.main()
