"""Reuse the current reversible SE5 package; prepare/publish, never deploy."""
import argparse
import hashlib
import json
import re
import shutil
from datetime import datetime
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def validate(package, commit=None):
    m = json.loads((package / "package.json").read_text(encoding="utf-8-sig"))
    if commit is not None and m["source_commit"] != commit:
        raise ValueError("Package source differs from requested source")
    for item in m["payload"]:
        if Path(item["name"]).name != item["name"] or digest(package / item["name"]) != item["sha256"]:
            raise ValueError("Package payload mismatch: " + item["name"])
    return m


def prepare(template, dll, documents, source, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("An exact source commit is required")
    old = validate(template)
    package = documents / ("NA-SE5-AMD-repair-" + commit[:7])
    package.mkdir()
    for item in old["payload"]:
        shutil.copy2(template / item["name"], package / item["name"])
    script = (package / "deploy.ps1").read_text(encoding="utf-8-sig")
    pattern = r"(?m)^\$TestCommit\s*=.*$"
    script, count = re.subn(pattern, "$TestCommit = '" + commit + "'", script)
    if count != 1:
        raise ValueError("Expected one source identifier in SE5 transaction")
    script = script.replace("flicker-diagnostic", "AMD flow repair").replace("flicker diagnostic", "AMD flow repair")
    # Preserve the current Results/rollback/capture commands and add the flow
    # evidence to the same saved result directory.
    marker = "        $cam | Set-Content"
    if marker not in script:
        raise ValueError("The current SE5 Results transaction was not found")
    script = script.replace(marker,
        "        @($lines | Where-Object { $_ -like 'j2compute*' }) | Set-Content -LiteralPath (Join-Path $dst ($s.BaseName + '-amd-flow.txt')) -Encoding UTF8\n" + marker, 1)
    script = script.replace("foreach ($p in '^j2 on=',", "foreach ($p in '^j2compute ', '^j2 on=',", 1)
    (package / "deploy.ps1").write_text(script, encoding="utf-8-sig")
    shutil.copy2(dll, package / "NeuralAliasDXGI.dll")
    shutil.copy2(source / "docs/SE5-AMD-FLOW-STALL-REPAIR.md", package / "ENGINEERING.md")
    (package / "README.txt").write_text(
        "NeuralAlias SE5 AMD flow repair " + commit + "\n"
        "Close SE5 and run NA-se5-dlaa.cmd or NA-se5-flow-update.cmd.\n"
        "Moving-object motion starts ON, matching the previous SE5 test package.\n"
        "The overlay shows paired/submitted frames and labels the last completed pixel sample and age.\n"
        "NA-se5-flow-results.cmd saves logs, including AMD rejection reasons and resource counts.\n"
        "NA-se5-flow-rollback.cmd restores the original installation using the existing receipt.\n"
        "Previous packages and launcher backups remain available. Native game DLLs are never replaced.\n"
        "Automated regression repair; actual SE5 visual acceptance remains Cort's game test.\n", encoding="utf-8")
    manifest = {"source_commit": commit, "base_commit": old["source_commit"], "payload": [
        {"name": p.name, "sha256": digest(p)} for p in sorted(package.iterdir()) if p.is_file() and p.name != "package.json"]}
    (package / "package.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    validate(package, commit)
    return package


def publish(documents, package, commit):
    validate(package, commit)  # validate ALL bytes before changing any launcher
    actions = {"dlaa": "Deploy", "flow-update": "Deploy", "camfix-update": "Deploy",
               "dlaa-rollback": "Rollback", "flow-rollback": "Rollback", "camfix-rollback": "Rollback",
               "flow-results": "Results", "camfix-results": "Results"}
    backup = documents / ("NA-SE5-launchers-before-" + commit[:7] + "-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    backup.mkdir()
    for suffix in actions:
        target = documents / ("NA-se5-" + suffix + ".cmd")
        if target.exists():
            shutil.copy2(target, backup / target.name)
    for suffix, action in actions.items():
        text = ('@echo off\r\n' + f'echo NeuralAlias SE5 - AMD flow repair {commit[:7]}\r\n' +
                f'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{package / "deploy.ps1"}" -Action {action}\r\n' + 'pause\r\n')
        (documents / ("NA-se5-" + suffix + ".cmd")).write_bytes(text.encode("utf-8"))
    return backup


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    for name in ("template", "dll", "documents", "source", "commit"):
        p.add_argument("--" + name, required=True)
    p.add_argument("--publish", action="store_true")
    a = p.parse_args()
    package = prepare(Path(a.template), Path(a.dll), Path(a.documents), Path(a.source), a.commit)
    print(package)
    if a.publish:
        print("Launcher backup:", publish(Path(a.documents), package, a.commit))
