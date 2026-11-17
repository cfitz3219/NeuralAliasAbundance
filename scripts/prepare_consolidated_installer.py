"""Stage one NeuralAlias installer using the existing six game transactions."""

import argparse
import json
import re
import shutil
from pathlib import Path

from prepare_se5_flow_repair import digest


GAMES = (
    ("se5", "Sniper Elite 5", "B", "NA-se5-motion-1ebb7b7",
     r"C:\Program Files (x86)\Steam\steamapps\common\Sniper Elite 5\bin",
     "Sniper5_dx12.exe", "j2-se5-dlaa"),
    ("ser", "Sniper Elite Resistance", "B", "NA-SER-AMD-flow-a450adf",
     r"D:\SteamLibrary\steamapps\common\Sniper Elite Resistance\bin",
     "SniperResistance_dx12.exe", "j2-ser-dlaa"),
    ("anno", "Anno 1800", "B", "NA-Anno-native-present-1174073",
     r"C:\Program Files (x86)\Steam\steamapps\common\Anno 1800\Bin\Win64",
     "Anno1800.exe", "j2-anno-flow"),
    ("wwii", "Call of Duty WWII (campaign)", "B", "NA-wwii-on12-fc0e6bd",
     r"D:\SteamLibrary\steamapps\common\Call of Duty WWII",
     "s2_sp64_ship.exe", "j2-wwii-on12"),
    ("ghost", "Ghost of Tsushima", "A", "NA-Tsushima-package-ca5299f",
     r"D:\SteamLibrary\steamapps\common\Ghost of Tsushima DIRECTOR'S CUT",
     "GhostOfTsushima.exe", "Tsushima"),
    ("pbz", "Phantom Blade Zero", "A", "NA-PBZ-package-f04343f",
     r"C:\Program Files (x86)\Steam\steamapps\common\PBZ\platform\steam",
     "PBZ.exe", "PBZ"),
)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def verified_file(package, manifest, name):
    path = package / name
    expected = next((item["sha256"] for item in manifest["payload"] if item["name"] == name), None)
    if not expected or digest(path).casefold() != expected.casefold():
        raise ValueError(f"Missing or changed template file: {path}")
    return path


def known_route_b_proxy(bin_path, logs):
    receipt = logs / "manifest.json"
    if not receipt.is_file():
        return None
    manifest = read_json(receipt)
    if manifest.get("restoredUtc"):
        return None
    for item in manifest["files"]:
        if Path(item["path"]) == bin_path / "dxgi.dll" and not item.get("existed"):
            return item["deployedHash"]
    return None


def route_b_script(text, commit, label):
    text, count = re.subn(r"(?m)^\$TestCommit\s*=.*$", f"$TestCommit = '{commit}'", text)
    if count != 1:
        raise ValueError(f"Missing build field for {label}")
    text = text.replace("[host]\n", "[host]\ngraphics_api=auto\nbuild_commit=$TestCommit\n", 1)
    text = re.sub(r"(?m)^tag=.*$", f"tag=NeuralAlias ({label})", text)
    text = re.sub(r"(?m)^(proxy|render_small|native_scale|scale_mode)=1$", r"\1=0", text)
    text = text.replace("[native11]\nenabled=1", "[native11]\nenabled=0")
    text = re.sub(r"(?m)^producers=0$", "producers=1", text)
    text = re.sub(r"(?m)^\s*\$sdkDll = Join-Path \(Split-Path -Parent \$PSScriptRoot\) .*$",
                  r"\n    $sdkDll = Join-Path $PSScriptRoot 'nvngx_dlss.dll'", text)
    return "# Combined DLAA package. Existing reversible transaction below.\n" + text[text.index("param("):]


def stage(args):
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        raise ValueError("The exact source commit is required")
    root = args.destination
    root.mkdir()
    index = {"source_commit": args.commit, "games": []}
    fallback_dlss = None
    for game, label, route, template_name, bin_name, exe, scope in GAMES:
        template = args.documents / template_name
        manifest = read_json(template / "package.json")
        directory = root / game
        directory.mkdir()
        bin_path = Path(bin_name)
        profile = dict(id=game, label=label, route=route, bin=bin_name, exe=exe, processes=[Path(exe).stem])
        known = {digest(verified_file(template, manifest, "NeuralAliasDXGI.dll"))}
        if route == "B":
            logs = args.local / scope
            current = known_route_b_proxy(bin_path, logs)
            if current:
                known.add(current)
            script = verified_file(template, manifest, "deploy.ps1").read_text(encoding="utf-8-sig")
            (directory / "deploy.ps1").write_text(route_b_script(script, args.commit, label), encoding="utf-8-sig")
            names = ["AMD-OpticalFlow-LICENSE.txt", "select-captures.ps1", "NAProfile.exe",
                     "NA-capture-luma.ps1", "summarize-run.ps1"]
            if game == "se5":
                fallback_dlss = verified_file(template, manifest, "nvngx_dlss.dll")
                profile["processes"].append("Sniper5_vulkan")
            if game == "wwii":
                profile["processes"].append("s2_mp64_ship")
            dlss = fallback_dlss if game == "wwii" else verified_file(template, manifest, "nvngx_dlss.dll")
            shutil.copy2(dlss, directory / "nvngx_dlss.dll")
        else:
            logs = args.user_data / scope
            profile["game_profile"] = scope
            names = ["nvngx_dlssnr.dll", "NVIDIA-NR-LICENSE.txt", "NAProfile.exe",
                     "Poise-Regular.ttf", "Poise-Medium.ttf", "Poise-OFL.txt"]
            script = verified_file(template, manifest, "deploy-pbz.ps1").read_text(encoding="utf-8-sig")
            script = script.replace("[host]\n", "[host]\ngraphics_api=auto\nbuild_commit=$($package.source_commit)\n", 1)
            (directory / "deploy-pbz.ps1").write_text(script, encoding="utf-8-sig")
            if game == "ghost":
                # Installed additive build 8b0ea8f, recorded in COMMS. The old
                # package directory is absent. Preserve the actual files in-place.
                known.add("33F14F6C24704343B094191D02A7423FE05A808F699D3E9E6CD231D5954D2132")
        for name in names:
            if any(item["name"] == name for item in manifest["payload"]):
                shutil.copy2(verified_file(template, manifest, name), directory / name)
        shutil.copy2(args.dll, directory / "NeuralAliasDXGI.dll")
        write_json(directory / "package.json", {
            "source_commit": args.commit,
            "template_commit": manifest["source_commit"],
            "payload": [{"name": file.name, "sha256": digest(file)} for file in sorted(directory.iterdir())],
        })
        profile["logs"] = str(logs)
        profile["known_proxies"] = sorted(item.upper() for item in known)
        index["games"].append(profile)
    write_json(root / "profiles.json", index)
    shutil.copy2(Path(__file__).with_name("consolidated-install.ps1"), root)
    launcher = root / f"Install-NeuralAlias-{args.commit[:7]}.cmd"
    launcher.write_text(
        '@echo off\n'
        'powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0consolidated-install.ps1" %*\n'
        'pause\n', encoding="ascii", newline="\r\n")
    (root / "README.txt").write_text(
        f"NeuralAlias combined build {args.commit}\n\n"
        f"Run {launcher.name} as administrator and choose the game, then I to install.\n"
        "Start the game normally and press Home once in gameplay. DX11/DX12 is selected automatically.\n"
        "Use the normal DX renderer if the game also offers Vulkan. Use campaign for WWII.\n"
        "After quitting, use L to save the ordinary logs. R restores the preceding installation.\n"
        "The six listed games share the runtime. Existing DLLs owned by the game remain in place.\n",
        encoding="utf-8")
    return launcher


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("documents", "local", "user-data", "dll", "destination"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--commit", required=True)
    print(stage(parser.parse_args()))
