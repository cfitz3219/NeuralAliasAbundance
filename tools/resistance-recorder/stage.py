"""Stage a verified replacement for the Resistance recorder using its existing installer."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest().upper()


def stage(previous, runtime, commit, output):
    if output.exists():
        raise ValueError(f"Destination already exists: {output}")
    old = json.loads((previous / "package.json").read_text(encoding="utf-8-sig"))
    if old["executable"] != "SniperResistance_dx12.exe" or old["flat_map_lighting_test"]:
        raise ValueError("Expected the Resistance free-look recorder template.")
    if not re.fullmatch("[a-f0-9]{40}", commit):
        raise ValueError("Expected the complete source commit.")
    with tempfile.TemporaryDirectory(dir=output.parent) as scratch:
        staged = Path(scratch) / "recorder"
        staged.mkdir()
        for item in old["payload"]:
            name = item["name"]
            if Path(name).name != name or sha(previous / name) != item["sha256"].upper():
                raise ValueError(f"Invalid prior recorder payload: {name}")
            shutil.copy2(previous / name, staged / name)
        target = staged / "NeuralAliasDXGI.dll"
        shutil.copyfile(runtime, target)
        subprocess.run([sys.executable, str(ROOT / "tools/verify-lighting-runtime.py"), str(target)], check=True)
        installer = staged / "deploy.ps1"
        script = installer.read_text(encoding="utf-8-sig")
        pin = "$TestCommit = '" + old["source_commit"] + "'"
        if script.count(pin) != 1:
            raise ValueError("Installer revision does not match the template.")
        installer.write_text(script.replace(pin, "$TestCommit = '" + commit + "'"), encoding="utf-8-sig", newline="\r\n")
        old["template_commit"], old["source_commit"] = old["source_commit"], commit
        for item in old["payload"]:
            item["sha256"] = sha(staged / item["name"])
        (staged / "package.json").write_text(json.dumps(old, indent=2) + "\n", encoding="utf-8")
        staged.rename(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("previous", type=Path)
    parser.add_argument("runtime", type=Path)
    parser.add_argument("commit")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    stage(args.previous, args.runtime, args.commit, args.output)
