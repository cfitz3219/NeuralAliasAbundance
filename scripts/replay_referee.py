#!/usr/bin/env python3
"""Replay saved DLAA captures through the same C++ referee and preview policy used in play."""
import argparse
import json
import struct
import subprocess
from pathlib import Path

import numpy as np

from capture_jitter_map import colour, plane


def image(folder, meta, name, width, height):
    desc = next((p for p in meta["planes"] if p["name"] == name), None)
    if not desc or (desc["width"], desc["height"]) != (width, height):
        return None, None
    source = folder / f"{meta['index']:05d}-{name}.raw"
    if not source.is_file() or source.stat().st_size != desc["bytes"]:
        return None, None
    rows, fmt = plane(folder, meta["index"], name, meta)
    return rows, fmt


def luma(rows, fmt, width, height):
    if rows is None:
        return None
    if fmt == 10:
        rgba = rows.view("<f2").reshape(height, width, 4)
        return sum(rgba[..., channel].astype("<f4") * weight
                   for channel, weight in enumerate((.2126, .7152, .0722)))
    rgb = rows.view("<f4").reshape(height, width, 4)[..., :3] if fmt == 2 else colour(rows, fmt)[0]
    if rgb is None:
        return None
    if fmt in (29, 91):
        rgb = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    return (rgb[..., 0] * .2126 + rgb[..., 1] * .7152 + rgb[..., 2] * .0722).astype("<f4")


def write_frame(stream, folder, meta, control):
    dlss = meta.get("dlss", {})
    if (meta.get("capture_kind") != "evaluation" or not meta.get("evaluation_succeeded")
            or meta.get("debug_view") or dlss.get("jitter_shifted")):
        return False
    width, height = meta["size"]
    if not (0 < width <= 8192 and 0 < height <= 8192):
        return False
    color = luma(*image(folder, meta, "color", width, height), width, height)
    if color is None:
        return False
    ordinal = meta["present_ordinal"]
    output = luma(*image(folder, meta, "dlssout", width, height), width, height)
    screen = None
    if (meta.get("frame_closed") and meta.get("present_closed_frame") == ordinal
            and meta.get("present_pairing") == "overlay_present"):
        desc = next((p for p in meta["planes"] if p["name"] == "present"), None)
        if desc and 0 < desc["width"] <= 8192 and 0 < desc["height"] <= 8192:
            dims = (desc["width"], desc["height"])
            screen = luma(*image(folder, meta, "present", *dims), *dims)
    if screen is not None:
        if control == "bypass" or (control == "alternating" and meta["index"] % 2):
            screen = color
        elif control in ("output", "alternating"):
            screen = output
    have_screen = output is not None and screen is not None

    rows, fmt = image(folder, meta, "motion", width, height)
    motion = None
    if rows is not None and fmt in (16, 34) and not dlss.get("mv_jittered"):
        motion = rows.view("<f4" if fmt == 16 else "<f2").astype("<f4", copy=False)
    view = meta.get("view", {})
    route = meta.get("route", 0)
    flags = int(have_screen) | (int(motion is not None) << 1) | (int(bool(meta.get("reset"))) << 2)
    if have_screen and screen.shape != (height, width):
        flags |= 8
    stream.write(struct.pack(
        "<5Q5I4f", ordinal, view.get("color", 0), view.get("depth", 0),
        meta.get("referee_boundary", meta.get("insertion_signature", 0)), meta.get("history_generation", 0),
        route if route in (1, 2) else 0, view.get("color_sub", 0), width, height, flags,
        *meta.get("jitter_px", (0, 0)), *meta.get("mv_scale", (1, 1)),
    ))
    if flags & 8:
        stream.write(struct.pack("<2I", screen.shape[1], screen.shape[0]))
    stream.write(color.tobytes())
    if have_screen:
        stream.write(output.tobytes())
        stream.write(screen.tobytes())
    if motion is not None:
        stream.write(motion.tobytes())
    return True


def field_replay(runner, folder, metas):
    """Send identities and original pitches. The test uploads the unmodified planes to the GPU."""
    lines = []
    previous = None
    epoch = 0
    for path in metas:
        m = json.loads(path.read_text(encoding="utf-8-sig"))
        ordinal = m.get("present_ordinal")
        if (m.get("capture_kind") != "evaluation" or not m.get("evaluation_succeeded")
                or not m.get("frame_closed") or m.get("debug_view")
                or m.get("present_pairing") != "overlay_present"
                or ordinal != m.get("inputs_frame") or ordinal != m.get("present_closed_frame")):
            continue
        planes = {p["name"]: p for p in m["planes"]}
        names = ("color", "dlssout", "present")
        if any(k not in planes or [planes[k]["width"], planes[k]["height"]] != m["size"]
               for k in names):
            continue
        identity = (m.get("pid"), m.get("view"), m.get("history_generation"), m["size"],
                    [planes[k]["dxgi_format"] for k in names], m.get("preset"))
        if identity != previous or m.get("reset") or m.get("camera_cut"):
            epoch += 1
        previous = identity
        jitter = m["jitter_px"]
        raster = m.get("raster_jitter_px")
        verified = (raster is not None and not m.get("dlss", {}).get("jitter_shifted")
                    and np.allclose(raster, jitter, rtol=0, atol=1 / 32768) and np.linalg.norm(raster) > 1 / 32768)
        row = [m["index"], ordinal, *m["size"], *jitter, int(verified), epoch]
        for k in names:
            row.extend((planes[k]["dxgi_format"], planes[k]["row_pitch"]))
        lines.append(" ".join(map(str, row)))
    if not lines:
        raise ValueError("no exact evaluation and presentation tuples")
    return subprocess.run([str(runner.resolve()), "--capture-field", str(folder.resolve())],
                          input="\n".join(lines) + "\n", text=True).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--runner", type=Path, required=True, help="built na_referee_replay executable")
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--control", choices=("actual", "bypass", "output", "alternating"), default="actual",
                        help="controls substitute captured input, output or alternating saved images for the matched screen")
    parser.add_argument("--gpu-field", action="store_true", help="use referee_gpu_test and the actual captured native textures")
    args = parser.parse_args()
    if args.frames < 1:
        parser.error("--frames must be positive")
    metas = sorted(args.folder.glob("*-meta.json"))[: args.frames]
    if not metas:
        parser.error("no captured frames found")
    if args.gpu_field:
        if args.control != "actual":
            parser.error("GPU field replay uses the actual presentation")
        return field_replay(args.runner, args.folder, metas)
    print(f"{args.folder.name}: screen={args.control} metadata_frames={len(metas)}", flush=True)
    with subprocess.Popen([str(args.runner.resolve())], stdin=subprocess.PIPE) as runner:
        kept = 0
        with runner.stdin:
            for path in metas:
                meta = json.loads(path.read_text(encoding="utf-8-sig"))
                kept += write_frame(runner.stdin, args.folder, meta, args.control)
        result = runner.wait()
    print(f"replayed={kept} skipped={len(metas) - kept}")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
