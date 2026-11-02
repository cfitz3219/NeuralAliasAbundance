import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("repair", Path(__file__).parents[1] / "prepare_se5_flow_repair.py")
repair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repair)


class RepairPackage(unittest.TestCase):
    def test_verified_launchers_preserve_previous_bytes_and_ser(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); docs = root / "Documents"; docs.mkdir(); package = docs / "package"; package.mkdir()
            (package / "deploy.ps1").write_text("test transaction")
            commit = "a" * 40
            (package / "package.json").write_text(json.dumps({"source_commit": commit, "payload": [
                {"name": "deploy.ps1", "sha256": repair.digest(package / "deploy.ps1")}] }))
            old = b"old\r\nlauncher\r\n"; (docs / "NA-se5-dlaa.cmd").write_bytes(old)
            (docs / "NA-ser-dlaa.cmd").write_bytes(b"SER stays")
            backup = repair.publish(docs, package, commit)
            self.assertEqual((backup / "NA-se5-dlaa.cmd").read_bytes(), old)
            self.assertIn(str(package).encode(), (docs / "NA-se5-dlaa.cmd").read_bytes())
            self.assertIn(b"-Action Results", (docs / "NA-se5-flow-results.cmd").read_bytes())
            self.assertEqual((docs / "NA-ser-dlaa.cmd").read_bytes(), b"SER stays")
            before = {x.name: x.read_bytes() for x in docs.glob("*.cmd")}
            (package / "deploy.ps1").write_text("corrupt")
            with self.assertRaises(ValueError):
                repair.publish(docs, package, commit)
            self.assertEqual(before, {x.name: x.read_bytes() for x in docs.glob("*.cmd")})


if __name__ == "__main__":
    unittest.main()
