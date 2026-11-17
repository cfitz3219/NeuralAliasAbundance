"""Command publication only: fake packages, no installer or game invocation."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "sniper_packages", Path(__file__).resolve().parents[1] / "prepare_sniper_flow_packages.py")
packages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packages)


class CommandTests(unittest.TestCase):
    def setUp(self):
        self.docs = Path(tempfile.mkdtemp(prefix="na-sniper-launchers-"))
        self.commit = "a" * 40
        self.old = {}
        for game in ("ser", "se5"):
            package = self.docs / f"NA-{game.upper()}-AMD-flow-{self.commit[:7]}"
            package.mkdir()
            payload = package / "deploy.ps1"
            payload.write_bytes(b"fixture only; must never execute")
            (package / "package.json").write_text(json.dumps({
                "source_commit": self.commit, "payload": [{"name": payload.name,
                "sha256": hashlib.sha256(payload.read_bytes()).hexdigest().upper()}]}))
            for suffix in ("flow-update", "flow-rollback", "dlaa", "dlaa-rollback"):
                target = self.docs / f"NA-{game}-{suffix}.cmd"
                self.old[target] = b"old command\r\n" + target.name.encode()
                target.write_bytes(self.old[target])

    def test_familiar_and_flow_commands_use_same_build_and_keep_backups(self):
        backup = packages.publish_commands(self.docs, self.commit)
        for target, old in self.old.items():
            self.assertEqual((backup / target.name).read_bytes(), old)
            text = target.read_text()
            self.assertIn(self.commit[:7], text)
            self.assertIn("AMD motion available", text)
            self.assertIn("Rollback" if "rollback" in target.name else "Deploy", text)
        for game in ("ser", "se5"):
            self.assertEqual((self.docs / f"NA-{game}-dlaa.cmd").read_bytes(),
                             (self.docs / f"NA-{game}-flow-update.cmd").read_bytes())

    def test_bad_second_package_leaves_every_old_command_unchanged(self):
        second = self.docs / f"NA-SE5-AMD-flow-{self.commit[:7]}" / "deploy.ps1"
        second.write_text("corrupt second candidate")
        with self.assertRaisesRegex(ValueError, "Payload mismatch"):
            packages.publish_commands(self.docs, self.commit)
        for target, old in self.old.items():
            self.assertEqual(target.read_bytes(), old)
        self.assertFalse(list(self.docs.glob("NA-Sniper-launchers-before-*")))


if __name__ == "__main__":
    unittest.main()
