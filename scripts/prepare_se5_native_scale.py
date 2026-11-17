"""Prepare the existing additive SE5 transaction with native scale control. Never deploy."""
import argparse
import json
import re
import shutil
from datetime import datetime
from pathlib import Path

from prepare_se5_flow_repair import digest, validate


def prepare(template, dll, documents, source, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("An exact runtime source commit is required")
    old = validate(template)
    package = documents / ("NA-SE5-native-scale-" + commit[:7])
    package.mkdir()
    for item in old["payload"]:
        shutil.copy2(template / item["name"], package / item["name"])
    script = (package / "deploy.ps1").read_text(encoding="utf-8-sig")
    script, count = re.subn(r"(?m)^\$TestCommit\s*=.*$", "$TestCommit = '" + commit + "'", script)
    if count != 1:
        raise ValueError("Expected one source identifier in the existing transaction")
    script = "# Native engine render-scale control " + commit + "; original transaction/provenance follows.\n" + script
    script, count = re.subn(r"(?m)^scene_resources=1\s*$", "scene_resources=0", script)
    if count != 1:
        raise ValueError("Expected the qualified scene transaction template")
    script, count = re.subn(r"(?m)^\[layer2\]\s*\nenabled=1\s*$", "[layer2]\nenabled=0", script)
    if count != 1:
        raise ValueError("Expected one previous scene-provider configuration")
    script, count = re.subn(r"(?m)^\[j2\]\s*\nenabled=1\s*$", "[j2]\nenabled=1\nnative_scale=1", script)
    if count != 1 or "render_small=0" not in script or "read_hook=0" not in script:
        raise ValueError("Native control must have no window virtualization or marker scan")
    (package / "deploy.ps1").write_text(script, encoding="utf-8-sig")
    shutil.copy2(dll, package / "NeuralAliasDXGI.dll")
    shutil.copy2(source / "docs/SNIPER-NATIVE-SCALE-CONTROL-20261106.md", package / "ENGINEERING.md")
    (package / "README.txt").write_text(
        "NeuralAlias SE5 native render-scale control " + commit + "\n\n"
        "Close the game and run NA-se5-dlss-update.cmd (the scene and DLAA launchers point here too).\n"
        "Home > Picture > Render mode changes DLAA, Quality, Balanced, Performance and Ultra Performance live.\n"
        "Keep your preferred windowed/borderless/fullscreen mode; the installer does not change it.\n"
        "The status confirms the real scene dimensions and a newer full-output DLSS submission.\n"
        "Native settings/update calls own resizing. User game settings are preserved; no numeric scans.\n"
        "The game's actual scene-upscale operation supplies the destination; later HUD drawing remains native.\n"
        "Existing camera/motion fixes through 71f27d1 are included. No additional experimental flow change.\n"
        "NA-se5-dlss-results.cmd collects the usual logs and frame captures.\n"
        "NA-se5-dlss-rollback.cmd restores the original installation; native-previous restores the prior updater.\n"
        "Automated CPU and real-DX12/fake-model evidence is documented in ENGINEERING.md.\n"
        "Actual game image quality/performance and window-mode acceptance remain the user's test.\n",
        encoding="utf-8")
    manifest = dict(old)
    manifest.update(source_commit=commit, base_commit=old["source_commit"], native_scale=True,
                    included_motion_commit="71f27d1")
    manifest["payload"] = [{"name": p.name, "sha256": digest(p)} for p in sorted(package.iterdir())
                           if p.is_file() and p.name != "package.json"]
    (package / "package.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    validate(package, commit)
    return package


def publish(documents, package, commit):
    validate(package, commit)
    actions = {"dlss-update": "Deploy", "scene-update": "Deploy", "dlaa": "Deploy",
               "dlss-results": "Results", "dlss-rollback": "Rollback", "scene-rollback": "Rollback"}
    backup = documents / ("NA-SE5-launchers-before-native-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    backup.mkdir()
    for suffix in actions:
        target = documents / ("NA-se5-" + suffix + ".cmd")
        if target.exists():
            shutil.copy2(target, backup / target.name)
    for suffix, action in actions.items():
        text = ("@echo off\r\n" + f"echo NeuralAlias SE5 native scale control {commit[:7]}. Close the game first.\r\n" +
                'set "NA_PS=powershell.exe"\r\nwhere pwsh.exe >nul 2>&1 && set "NA_PS=pwsh.exe"\r\n' +
                f'"%NA_PS%" -NoProfile -ExecutionPolicy Bypass -File "{package / "deploy.ps1"}" -Action {action}\r\n' +
                "echo.\r\npause\r\n")
        (documents / ("NA-se5-" + suffix + ".cmd")).write_bytes(text.encode("utf-8"))
    for suffix, previous in (("native-previous", "dlss-update"), ("scene-previous", "scene-update")):
        old = backup / ("NA-se5-" + previous + ".cmd")
        if old.exists():
            shutil.copy2(old, documents / ("NA-se5-" + suffix + ".cmd"))
    return backup


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("template", "dll", "documents", "source", "commit"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--publish", action="store_true")
    options = parser.parse_args()
    candidate = prepare(Path(options.template), Path(options.dll), Path(options.documents), Path(options.source), options.commit)
    print(candidate)
    if options.publish:
        print("Launcher backup:", publish(Path(options.documents), candidate, options.commit))
