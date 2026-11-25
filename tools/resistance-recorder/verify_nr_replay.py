"""Run the offline Resistance probe and export a matched final-frame picture on success."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

import numpy as np
from PIL import Image


def verify(preset, package, frames, output):
    with tempfile.TemporaryDirectory(prefix="nr-replay-", dir=output) as work:
        work = Path(work)
        # The probe reads fixture files and writes its output in the same directory.
        # Hard links give each run a fresh output without duplicating the captures.
        for i in range(9):
            for name in ("color", "depth", "motion"):
                filename = f"{i}-{name}.bin"
                try:
                    os.link(frames / filename, work / filename)
                except OSError:
                    shutil.copyfile(frames / filename, work / filename)
        log_path = output / f"preset-{preset}.log"
        with log_path.open("w", encoding="utf-8") as log:
            run = subprocess.run(
                [str(package / "offline-nr-probe.exe"), "offline", str(package), str(work), str(preset)],
                cwd=work, stdout=log, stderr=subprocess.STDOUT, timeout=120, check=False,
            )
        log = log_path.read_text(encoding="utf-8", errors="replace")
        info = {"preset": preset, "returncode": run.returncode, "log": log_path.name}
        frames_ok = re.findall(r"^frame (\d+) evaluate 0x00000001\b", log, re.MULTILINE)
        if run.returncode or frames_ok != [str(i) for i in range(9)]:
            info["result"] = ("driver refused NR" if "please update display driver" in log
                              else "replay incomplete")
            return info

        w, h = 3440, 1440
        before = np.fromfile(work / "8-color.bin", "u1").reshape(h, w, 4)[:, :, :3]
        after = np.fromfile(work / f"after-{preset}.raw", "<f4").reshape(h, w, 4)[:, :, :3]
        if not np.isfinite(after).all():
            raise ValueError("NR produced non-finite RGB samples")
        after = np.rint(np.clip(after, 0, 1) * 255).astype("uint8")
        difference = np.abs(after.astype("int16") - before)
        info["changed_pixel_percent"] = float(100 * difference.any(axis=2).mean())
        info["mean_abs_rgb_codes"] = float(difference.mean())
        info["gpu_ms_per_frame"] = [float(t) for t in re.findall(r"model_ms ([0-9.]+)", log)]
        Image.fromarray(before).save(output / "before.png")
        Image.fromarray(after).save(output / f"after-preset-{preset}.png")
        info["result"] = "changed pixels" if difference.any() else "identical display pixels"
        return info


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("frames", type=Path)
    parser.add_argument("output", type=Path, help="Existing parent folder for independent replay results")
    args = parser.parse_args()
    package = args.package.resolve()
    with (package / "nvngx_dlssnr.dll").open("rb") as dll:
        if hashlib.file_digest(dll, "sha256").hexdigest() != "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f":
            raise ValueError("Unexpected NR model")
    out = Path(tempfile.mkdtemp(prefix="results-", dir=args.output.resolve()))
    results = [verify(p, package, args.frames.resolve(), out) for p in (1, 2)]
    (out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(out)
    for row in results:
        print(f"Preset {row['preset']} - {row['result']}")
    raise SystemExit(0 if all(r["result"] == "changed pixels" for r in results) else 1)
