"""Run the GameCheck dashboard on this computer and import saved results."""

from datetime import date, datetime
from html import escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
from tempfile import NamedTemporaryFile
from threading import Lock
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import urlopen
import webbrowser

import compare
import ingest
import manifest
import report
import store
from rules._evidence import number, status_hash

HOST = "127.0.0.1"
PORT = 8765
APP_MARKER = "GameCheck local dashboard"


def pick_folder():
    # A separate process keeps the Windows folder picker on its own UI thread.
    python = Path(sys.executable)
    if python.name.lower() == "pythonw.exe":
        python = python.with_name("python.exe")
    picked = subprocess.run(
        [str(python), __file__, "--pick-folder"],
        capture_output=True, text=True, encoding="utf-8",
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), check=True,
    )
    return picked.stdout.rstrip("\r\n")


def folder_dialog():
    from tkinter import Tk, filedialog

    root = Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    try:
        documents = Path.home() / "Documents"
        folder = filedialog.askdirectory(
            parent=root, title="Select the folder saved by the game's Results CMD",
            initialdir=str(documents if documents.is_dir() else Path.home()), mustexist=True,
        )
        sys.stdout.buffer.write(folder.encode("utf-8"))
    finally:
        root.destroy()


def save_result(path, record):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    scratch = None
    try:
        with NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as temp:
            scratch = Path(temp.name)
            temp.write(store.dump(record))
        scratch.replace(path)
    finally:
        if scratch:
            scratch.unlink(missing_ok=True)


class Library:
    def __init__(self, history, bundled=store.GROWTH):
        self.history = history
        self.bundled = bundled
        self.cases = store.cases(bundled)
        self.floors = {case: store.floor(case, bundled) for case in self.cases}

    def records(self):
        # An imported record takes precedence over a bundled copy of the run.
        records = list({
            (status_hash(result) or result["manifest"]["evidence_id"], result["manifest"]["case"]): result
            for folder in (self.bundled, self.history) for result in store.results(folder)
        }.values())
        for result in records:
            path = self.observation_path(result)
            if path.is_file():
                observation = store.load(path)[-1]
                result["jobs"]["continuity"] = {
                    "state": observation["state"], "source": "owner_verdict",
                    "note": "Player report: " + observation["note"], "evidence": [],
                }
        return records

    def observation_path(self, result):
        receipt = result["manifest"]
        return Path(self.history) / "observations" / receipt["case"] / (receipt["evidence_id"] + ".json")

    def observe(self, evidence, case, state, note):
        if state not in {"failed", "verified", "unknown"}:
            raise ValueError("Invalid observation")
        result = next((r for r in self.records() if r["manifest"]["evidence_id"] == evidence
                       and r["manifest"]["case"] == case and r["route"] == "B"), None)
        if result is None:
            raise ValueError("Unknown run")
        path = self.observation_path(result)
        history = store.load(path) if path.is_file() else []
        history.append({"state": state, "note": note[:1000],
                        "recorded_at": datetime.now().astimezone().isoformat(timespec="seconds")})
        save_result(path, history)
        return result["manifest"]["producer"]["build"]

    def detect_case(self, folder):
        match = manifest.FOLDER_NAME.fullmatch(Path(folder).name)
        choices = [case for case in self.cases.values()
                   if match and case["profile"] == match["profile"]]
        return choices[0] if len(choices) == 1 else None

    def import_folder(self, folder, case):
        existing = {(status_hash(r), r["manifest"]["case"]) for r in self.records()}
        added, repeated, skipped, builds = 0, 0, [], {}
        for result in ingest.ingest(folder, case):
            if result["status"] != ingest.SCORED:
                if result["status"] != ingest.EMPTY:
                    skipped.append(result["status_reason"])
                continue
            receipt = result["manifest"]
            build = receipt["producer"]["build"]
            if not build:
                skipped.append("A run has no identifiable build. Save results with its matching Results CMD.")
                continue
            if any("counters look like" in note for note in receipt["notes"]):
                skipped.append("A run's graphics provider disagrees with the selected game case.")
                continue
            builds[build] = max(builds.get(build, ""), receipt["session"].get("saved_at") or "")
            identity = status_hash(result), case["id"]
            if identity in existing:
                repeated += 1
                continue
            receipt["imported_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
            save_result(store.result_path(result, self.history), result)
            existing.add(identity)
            added += 1
        if not (added or repeated):
            return " ".join(dict.fromkeys(skipped)) or "This folder has no scored gameplay runs.", None
        details = [f"{case['game']} · build {', '.join(b[:8] for b in sorted(builds))}."]
        if added:
            details.append(f"Imported {added} new {'run' if added == 1 else 'runs'}.")
        if repeated:
            details.append(f"Already in history: {repeated}.")
        if skipped:
            details.append(" ".join(dict.fromkeys(skipped)))
        return " ".join(details), max(builds, key=lambda b: (builds[b], b))


def dashboard(library, token, selected="", notice="", needs_case=False):
    results = [r for r in library.records() if r["manifest"]["producer"]["build"]]
    build_dates = {}
    for result in results:
        m = result["manifest"]
        b = m["producer"]["build"]
        build_dates[b] = max(build_dates.get(b, ""), m["session"].get("saved_at") or "")
    builds = sorted(build_dates, key=lambda b: (build_dates[b], b), reverse=True)
    if selected not in builds:
        selected = builds[0] if builds else ""
    options = "".join(f'<option value="{escape(b)}"{" selected" if b == selected else ""}>'
                      f'{escape(b[:8])} · {escape(build_dates[b][:8] or "saved run")}</option>' for b in builds)
    csrf = f'<input type="hidden" name="token" value="{token}">'
    if needs_case:
        games = "".join(f'<option value="{escape(c["id"])}">{escape(c["game"])}</option>'
                        for c in sorted(library.cases.values(), key=lambda c: c["game"]))
        importer = (f'<form method="post" action="/import">{csrf}<label for="game">Which game?</label>'
                    f'<select id="game" name="game" required><option value="">Select a game</option>{games}</select>'
                    '<button class="primary">Import selected game</button>'
                    '<button name="cancel" value="1" formnovalidate>Cancel</button></form>')
    else:
        importer = f'<form method="post" action="/import">{csrf}<button class="primary">Import Results</button></form>'
    chooser = (f'<form method="get"><label for="build">Build history</label>'
               f'<select id="build" name="build">{options}</select><button>View build</button></form>'
               if options else "")
    history = []
    for r in results:
        m = r["manifest"]
        if not manifest.build_matches(m["producer"]["build"], selected):
            continue
        game = library.cases.get(m["case"], {}).get("game", m["case"] or "Unassigned")
        when = m["session"].get("saved_at") or m.get("imported_at", "")[:10] or "Date unavailable"
        fps = number(r, "fps_play_median")
        coverage = number(r, "delivered_share_of_presented")
        metrics = [f"{fps:.1f} FPS" if fps is not None else "",
                   f"{coverage:.1f}% of presented frames delivered" if coverage is not None else ""]
        history.append((when, game, " · ".join(filter(None, metrics)) or "See full evidence"))
    rows = "".join(f"<li><b>{escape(game)}</b> · {escape(when)} · {escape(metrics)}</li>"
                   for when, game, metrics in sorted(history, reverse=True))
    saved = f'<details><summary>Saved runs for this build ({len(history)})</summary><ul>{rows}</ul></details>' if rows else ""
    run_options = []
    for r in results:
        m = r["manifest"]
        if r["route"] != "B" or not manifest.build_matches(m["producer"]["build"], selected):
            continue
        identity = m["case"] + "|" + m["evidence_id"]
        name = library.cases[m["case"]]["game"] + " · " + (m["session"].get("saved_at") or m["evidence_id"])
        run_options.append(f'<option value="{escape(identity)}">{escape(name)}</option>')
    observations = ""
    if run_options:
        observations = (f'<details><summary>Record what you saw</summary><form method="post" action="/observe">{csrf}'
                        '<p><label>Saved run <select name="run">' + "".join(run_options) + '</select></label></p>'
                        '<p><label>Visual continuity <select name="state">'
                        '<option value="failed">DLAA flickered or switched on/off</option>'
                        '<option value="verified">DLAA stayed visibly stable</option>'
                        '<option value="unknown">I could not judge stability</option></select></label></p>'
                        '<p><label>Your notes <input name="note" maxlength="1000"></label></p>'
                        '<button>Save observation</button></form></details>')
    status = f'<p role="status">{escape(notice)}</p>' if notice else ""
    controls = f'''
<section class="local-controls" aria-label="Manage results">
<div class="local-actions">{importer}{chooser}
<form method="post" action="/close">{csrf}<button>Close GameCheck</button></form></div>
{status}{observations}{saved}</section>
<style>
.local-controls {{ padding: 16px; background: var(--sheet); border: 1px solid var(--line); border-radius: 14px; }}
.local-actions, .local-actions form {{ display: flex; flex-wrap: wrap; align-items: center; gap: 8px; margin: 0; }}
.local-actions {{ gap: 12px 24px; }}
.local-actions button, .local-actions select {{ font: inherit; font-weight: 600; color: var(--ink); background: var(--paper);
    border: 1px solid var(--line); border-radius: 8px; padding: 7px 11px; }}
.local-actions button {{ cursor: pointer; }}
.local-actions button.primary {{ color: #fff; background: #245cc9; border-color: #245cc9; }}
.local-controls [role=status] {{ padding: 8px 12px; border-left: 3px solid var(--accent); }}
</style>'''
    comparison = compare.compare_build(selected or "—", library.cases, results, library.floors)
    return ('<!doctype html><html lang="en"><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1">'
            + report.page(comparison, library.cases, results, date.today().isoformat(),
                          controls=controls, external_fonts=False) + "</html>")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        if sys.stderr is not None:
            super().log_message(format, *args)

    def valid_host(self):
        return self.headers.get("Host") == self.server.address

    def send_body(self, content, kind="text/html; charset=utf-8"):
        data = content.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; "
                         "form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if not self.valid_host():
            return self.send_error(403)
        url = urlsplit(self.path)
        if url.path == "/health":
            return self.send_body(APP_MARKER, "text/plain")
        if url.path != "/":
            return self.send_error(404)
        build = parse_qs(url.query).get("build", [""])[0]
        notice = self.server.notice
        self.server.notice = ""
        self.send_body(dashboard(self.server.library, self.server.token, build, notice,
                                 bool(self.server.pending)))

    def redirect(self, build=""):
        self.send_response(303)
        self.send_header("Location", "/?" + urlencode({"build": build}) if build else "/")
        self.end_headers()

    def do_POST(self):
        if not self.valid_host() or self.headers.get("Origin") != "http://" + self.server.address:
            return self.send_error(403)
        size = self.headers.get("Content-Length", "")
        if not size.isdigit() or int(size) > 4096:
            return self.send_error(400)
        form = parse_qs(self.rfile.read(int(size)).decode("ascii", errors="replace"))
        token = form.get("token", [""])[0]
        if not token.isascii() or not secrets.compare_digest(token, self.server.token):
            return self.send_error(403)
        if self.path == "/close":
            if self.server.import_lock.locked():
                return self.send_error(409, "Finish the current import before closing GameCheck.")
            self.send_body("<title>GameCheck closed</title><p>GameCheck is closed. Your saved runs are available next time.</p>")
            self.server.shutdown()
            return
        if self.path not in {"/import", "/observe"}:
            return self.send_error(404)
        if not self.server.import_lock.acquire(blocking=False):
            return self.send_error(409, "Finish the current import first.")
        selected = ""
        try:
            if self.path == "/observe":
                case, evidence = form.get("run", [""])[0].split("|", 1)
                state = form.get("state", [""])[0]
                default_note = {"failed": "DLAA flickered or switched on/off.",
                                "verified": "DLAA stayed visibly stable.", "unknown": "Stability was not judged."}
                selected = self.server.library.observe(evidence, case, state,
                            form.get("note", [""])[0].strip() or default_note.get(state, ""))
                self.server.notice = "Your observation was saved with this run."
                return self.redirect(selected)
            if form.get("cancel"):
                self.server.pending = ""
                return self.redirect()
            folder = self.server.pending or pick_folder()
            if folder:
                library = self.server.library
                case = library.cases.get(form.get("game", [""])[0]) if self.server.pending else library.detect_case(folder)
                if case:
                    self.server.pending = ""
                    self.server.notice, selected = library.import_folder(folder, case)
                else:
                    self.server.pending = folder
                    self.server.notice = "Choose the game for the folder you selected."
        except (OSError, ValueError, subprocess.SubprocessError):
            self.server.notice = "GameCheck could not read or save these results. Check the folder and try again."
        finally:
            self.server.import_lock.release()
        self.redirect(selected)


class GameCheckServer(ThreadingHTTPServer):
    allow_reuse_address = False

    def server_bind(self):
        if os.name == "nt":
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()

    def __init__(self, library, port=PORT):
        super().__init__((HOST, port), Handler)
        self.library = library
        self.address = f"{HOST}:{self.server_port}"
        self.token = secrets.token_urlsafe(24)
        self.notice = ""
        self.pending = ""
        self.import_lock = Lock()


def main():
    history = Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "GameCheck"
    try:
        server = GameCheckServer(Library(history))
    except OSError:
        address = f"http://{HOST}:{PORT}"
        with urlopen(address + "/health", timeout=2) as response:
            if response.read().decode("utf-8") != APP_MARKER:
                raise RuntimeError("Another application is using GameCheck's local address.")
        webbrowser.open(address)
        return
    with server:
        webbrowser.open("http://" + server.address)
        server.serve_forever()


if __name__ == "__main__":
    if "--pick-folder" in sys.argv:
        folder_dialog()
    else:
        try:
            main()
        except Exception as error:
            if os.name == "nt":
                import ctypes
                ctypes.windll.user32.MessageBoxW(None, str(error), "GameCheck could not start", 0x10)
            else:
                raise
