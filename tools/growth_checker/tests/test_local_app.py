import http.client
from pathlib import Path
from tempfile import TemporaryDirectory
from threading import Thread
import unittest
from unittest.mock import patch
from urllib.parse import urlencode

import local_app
import store
import synthetic


class LocalGameCheck(unittest.TestCase):
    def test_import_duplicate_restart_and_case_selection(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            history = root / "history"
            build = "a" * 40
            saved = root / "NA-wwii-results-20261111-120500"
            saved.mkdir()
            log = synthetic.steady(40, build=build)
            (saved / "status-1.txt").write_text(log, encoding="utf-8")
            library = local_app.Library(history)
            floor = store.dump(library.floors)
            self.assertIn("Imported 1 new run", library.import_folder(saved, library.detect_case(saved))[0])

            reopened = local_app.Library(history)
            self.assertEqual(len(list(history.glob("results/*/*/*.json"))), 1)
            self.assertIn("Already in history: 1", reopened.import_folder(saved, reopened.detect_case(saved))[0])
            self.assertEqual(store.dump(reopened.floors), floor)
            record = next(r for r in reopened.records() if r["manifest"]["producer"]["build"] == build)
            reopened.observe(record["manifest"]["evidence_id"], "wwii-campaign-dlaa",
                             "failed", "Rapid on/off flicker.")
            reopened.import_folder(saved, reopened.detect_case(saved))
            reopened = local_app.Library(history)
            page = local_app.dashboard(reopened, "token", build)
            self.assertIn("Flickering reported", page)
            self.assertIn("Rapid on/off flicker.", page)
            self.assertNotIn("of gameplay frames", page)
            self.assertNotIn("DLAA frame coverage", page)

            other = root / "Custom folder"
            other.mkdir()
            second_build = "b" * 40
            (other / "status-2.txt").write_text(synthetic.steady(40, build=second_build), encoding="utf-8")
            with local_app.GameCheckServer(reopened, port=0) as server:
                with self.assertRaises(OSError):
                    local_app.GameCheckServer(reopened, port=server.server_port)
                worker = Thread(target=server.serve_forever, daemon=True)
                worker.start()
                self.addCleanup(worker.join, 5)
                try:
                    origin = "http://" + server.address
                    with patch.object(local_app, "pick_folder", return_value=str(other)):
                        status, _ = self.request(server, "POST", "/import", origin=origin, token=server.token)
                        self.assertEqual(status, 303)
                        self.assertIn("Which game?", self.request(server, "GET", "/")[1])
                        status, _ = self.request(server, "POST", "/import", origin=origin, token=server.token,
                                                 game="wwii-campaign-dlaa")
                        self.assertEqual(status, 303)
                    html = self.request(server, "GET", "/?build=" + second_build)[1]
                    self.assertIn("Import Results", html)
                    self.assertIn("Imported 1 new run", html)
                    self.assertIn("Saved runs for this build (1)", html)
                    self.assertIn(build, html)
                    self.assertNotIn("fonts.googleapis.com", html)
                    self.assertEqual(self.request(server, "POST", "/import", origin="https://example.org",
                                                  token=server.token)[0], 403)
                finally:
                    server.shutdown()
                    worker.join(5)
            self.assertEqual(len(list(history.glob("results/*/*/*.json"))), 2)

    def request(self, server, method, path, origin=None, **form):
        connection = http.client.HTTPConnection(server.address, timeout=5)
        try:
            headers = {"Origin": origin, "Content-Type": "application/x-www-form-urlencoded"} if origin else {}
            connection.request(method, path, urlencode(form) if form else None, headers)
            result = connection.getresponse()
            return result.status, result.read().decode("utf-8")
        finally:
            connection.close()
