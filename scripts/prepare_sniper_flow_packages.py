"""Prepare prebuilt Sniper candidates using the existing add-only installers.

Does not launch/install into a game. Retains the existing transaction, settings
carry-over, original-file snapshot and rollback; replaces only git/build work
with a hash-checked immutable payload so testing needs no fresh build.
"""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def prepare(docs, dll, runtime, source, commit):
    if len(commit) != 40 or any(c not in "0123456789abcdef" for c in commit):
        raise ValueError("Expected exact source commit")
    for game in ("ser", "se5"):
        old = (docs / f"NA-{game}-dlaa.ps1").read_text(encoding="utf-8-sig")
        package = docs / f"NA-{game.upper()}-AMD-flow-{commit[:7]}"
        if package.exists():
            raise FileExistsError(package)
        package.mkdir()
        script = old[old.index("param("):]
        import re
        script = re.sub(r"(?m)^\$Branch\s*=.*$", "$Branch = 'astra/sniper-amd-flow'", script)
        script = re.sub(r"(?m)^\$TestCommit\s*=.*$", f"$TestCommit = '{commit}'", script)
        start = script.index('    Log "Fetching $Branch"')
        end = script.index("    $ini = Join-Path $Evid", start)
        script = script[:start] + '''    $commit = $TestCommit
    $dll = Join-Path $PSScriptRoot 'NeuralAliasDXGI.dll'
    $sdkDll = Join-Path $PSScriptRoot 'nvngx_dlss.dll'
    Log "Using prebuilt AMD-flow candidate $commit (no rebuild)."

''' + script[end:]
        validation = '''    $package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package.json') -Raw | ConvertFrom-Json
    if($package.source_commit -ne $TestCommit) { throw 'Wrong package source; existing installation unchanged.' }
    foreach($payload in $package.payload) {
        if((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $payload.name)).Hash -ne $payload.sha256) {
            throw "Package mismatch: $($payload.name). Existing installation unchanged."
        }
    }
'''
        pos = script.index("    if (Test-Path -LiteralPath $Manifest) {")
        script = script[:pos] + validation + script[pos:]
        script = script.replace("foreach ($n in 'dxgi.dll', 'NeuralAliasHost.ini')", "foreach ($n in 'dxgi.dll', 'NeuralAliasHost.ini', 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt')")
        script = script.replace("[j2]\nenabled=1", "[j2]\nenabled=1\n; Enable Moving-object motion (test) in Home for the AMD comparison.\nflow=0")
        script = script.replace("'NeuralAliasHost-status.log')", "'NeuralAliasHost-status.log', 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt')")
        script = script.replace("'nvngx_dlss.dll' { $sdkDll }", "'nvngx_dlss.dll' { $sdkDll } 'NeuralAlias-AMD-OpticalFlow-LICENSE.txt' { (Join-Path $PSScriptRoot 'AMD-OpticalFlow-LICENSE.txt') }")
        script = script.replace("    Log 'READY.", "    Write-Host 'Home > Moving-object motion (test): turn ON to test AMD flow; turn OFF for camera-only comparison.'\n    Log 'READY.")
        (package / "deploy.ps1").write_text("# Prebuilt AMD flow; existing Sniper additive transaction retained.\n" + script, encoding="utf-8")
        (package / "NeuralAliasDXGI.dll").write_bytes(dll.read_bytes())
        (package / "nvngx_dlss.dll").write_bytes(runtime.read_bytes())
        (package / "AMD-OpticalFlow-LICENSE.txt").write_bytes((source / "third_party/ffx-opticalflow/LICENSE.txt").read_bytes())
        (package / "ENGINEERING.md").write_bytes((source / "docs/SNIPER-AMD-OPTICAL-FLOW.md").read_bytes())
        (package / "README.txt").write_text(
            f"AMD same-frame optical-flow candidate {commit}\nClose the game and use NA-{game}-flow-update.cmd.\n"
            "Uses the previous Sniper install receipt for rollback/settings carry-over; no native game DLL replacement.\n"
            "Home > Moving-object motion (test) starts OFF. Enable it to compare moving characters, then disable for baseline.\n"
            "See j2compute in the status log for AMD pairs/accepted pixels/time. Cort retains game visual testing.\n"
            "Installer builds nothing and does not fetch or change repository branches. NVIDIA DLSS comes from your local SDK.\n",
            encoding="utf-8")
        (package / "package.json").write_text(json.dumps({"source_commit": commit, "payload": [
            {"name": f.name, "sha256": digest(f)} for f in sorted(package.iterdir()) if f.is_file()
        ]}, indent=2), encoding="utf-8")
        for action, verb in (("update", "Deploy"), ("rollback", "Rollback")):
            target = docs / f"NA-{game}-flow-{action}.cmd"
            target.write_text('@echo off\n' + f'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{package / "deploy.ps1"}" -Action {verb}\npause\n', encoding="utf-8")
        print(package)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    for name in ("documents", "dll", "runtime", "source", "commit"):
        p.add_argument("--" + name, required=True)
    a = p.parse_args()
    prepare(Path(a.documents), Path(a.dll), Path(a.runtime), Path(a.source), a.commit)
