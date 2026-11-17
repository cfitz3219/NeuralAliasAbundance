#!/usr/bin/env python3
"""Does the picture DLAA receives really move with the jitter? Read it from a saved capture.

A capture folder (menu: "Capture DLAA frames") holds, per frame, what DLAA was given (color,
depth, motion), what it produced (dlssout), the picture that was shown (present) and the jitter
DLAA was told (meta.json). With the camera still, the only thing that should change between two
inputs is the jitter. This tool measures, block by block, how much of the told shift is really
in the picture:

  1.0  the content moved by exactly the told jitter (DLAA can rebuild its edges)
  0.0  the content did not move at all (drawn without the jitter: DLAA gains nothing there)

It also reports whether DLAA's output is steady from frame to frame while its input moves,
whether the shown picture follows the output or the input, and whether depth and motion hold
anything. Needs numpy; Pillow only for the map picture.

  python3 scripts/capture_jitter_map.py <capture folder> [--map out.png] [--frames 8]
"""
import argparse
import glob
import json
import os
import sys

import numpy as np

BYTES = {28: 4, 29: 4, 87: 4, 91: 4, 88: 4, 24: 4, 26: 4, 10: 8, 41: 4, 16: 8, 2: 16}


def plane(folder, index, name, meta):
    info = next((p for p in meta["planes"] if p["name"] == name), None)
    path = os.path.join(folder, "%05d-%s.raw" % (index, name))
    if not info or not os.path.exists(path):
        return None, None
    w, h, fmt, pitch = info["width"], info["height"], info["dxgi_format"], info["row_pitch"]
    bpp = BYTES.get(fmt)
    if not bpp:
        return None, fmt
    raw = np.fromfile(path, dtype=np.uint8)
    if raw.size < h * pitch:  # the last row is written without its padding
        raw = np.concatenate([raw, np.zeros(h * pitch - raw.size, np.uint8)])
    return np.ascontiguousarray(raw[: h * pitch].reshape(h, pitch)[:, : w * bpp]), fmt


def small_float(bits, mantissa_bits):
    m = (bits & ((1 << mantissa_bits) - 1)).astype(np.float64)
    e = (bits >> mantissa_bits).astype(np.int64)
    scale = float(1 << mantissa_bits)
    value = np.where(e == 0, m / scale * 2.0 ** -14, (1 + m / scale) * 2.0 ** (e - 15.0))
    return np.where(e == 31, 65504.0, value)


def colour(rows, fmt):
    """Rows of bytes -> (H, W, 3) float, and whether the values are scene-linear (HDR)."""
    h = rows.shape[0]
    if fmt in (28, 29):
        return rows.reshape(h, -1, 4)[..., :3].astype(np.float64) / 255.0, False
    if fmt in (87, 91, 88):
        return rows.reshape(h, -1, 4)[..., 2::-1].astype(np.float64) / 255.0, False
    if fmt == 24:
        v = rows.view(np.uint32).reshape(h, -1)
        return np.stack([(v >> s & 1023) / 1023.0 for s in (0, 10, 20)], -1), False
    if fmt == 26:
        v = rows.view(np.uint32).reshape(h, -1)
        return np.stack([small_float(v & 2047, 6), small_float(v >> 11 & 2047, 6), small_float(v >> 22, 5)], -1), True
    if fmt == 10:
        return rows.view(np.float16).reshape(h, -1, 4)[..., :3].astype(np.float64), True
    return None, False


def view_luma(rgb, hdr):
    """One brightness number per pixel on a display-like scale, so bright sky does not swamp the rest."""
    y = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]
    y = np.nan_to_num(np.maximum(y, 0.0), posinf=65504.0)
    if hdr:
        y = y / max(np.percentile(y, 90), 1e-6)
        y = y / (1.0 + y)
    return y * 255.0


def blur(a):
    k = np.array([1, 4, 6, 4, 1]) / 16.0
    a = sum(k[i] * np.roll(a, i - 2, 1) for i in range(5))
    return sum(k[i] * np.roll(a, i - 2, 0) for i in range(5))


def box(a, n):
    c = np.cumsum(np.cumsum(np.pad(a, ((1, 0), (1, 0))), 0), 1)
    return c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--map", help="write a picture: green moved with the jitter, red did not")
    ap.add_argument("--frames", type=int, default=8, help="how many frames to read (default 8: one jitter cycle)")
    ap.add_argument("--block", type=int, default=40)
    a = ap.parse_args()

    metas = sorted(glob.glob(os.path.join(a.folder, "*-meta.json")))[: a.frames]
    if len(metas) < 2:
        sys.exit("need at least two captured frames with meta.json")
    frames = []
    for path in metas:
        meta = json.load(open(path))
        index = meta["index"]
        rows, fmt = plane(a.folder, index, "color", meta)
        if rows is None:
            continue
        rgb, hdr = colour(rows, fmt)
        if rgb is None:
            sys.exit("colour format %s is not read by this tool yet" % fmt)
        cam = meta.get("camera", {})
        frames.append({"index": index, "meta": meta, "fmt": fmt, "hdr": hdr, "y": blur(view_luma(rgb, hdr)),
                       "jitter": np.array(meta["jitter_px"], float),
                       "still": cam.get("vp_current_row") == cam.get("vp_previous_row"), "rgb": rgb})
    if len(frames) < 2:
        sys.exit("fewer than two frames have a color plane on disk (stage the *-color.raw files)")
    first = frames[0]
    print("capture: %s  frames read: %d  size: %dx%d  colour format: %d (%s)  preset %s  sharpness %s" % (
        os.path.basename(os.path.normpath(a.folder)), len(frames), first["y"].shape[1], first["y"].shape[0], first["fmt"],
        "scene-linear" if first["hdr"] else "display", first["meta"].get("preset"), first["meta"].get("sharpness")))
    moving = [f["index"] for f in frames[1:] if not f["still"]]
    if moving:
        print("NOTE: the camera moved on frames %s; camera motion is mixed into the numbers below. A still camera gives the clean answer." % moving)
    told = [np.hypot(*f["jitter"]) for f in frames]
    if max(told) < 0.01:
        print("NOTE: the told jitter is zero on every frame: camera jitter was off when this was captured.")

    # 1. how much of the told shift is in the picture, block by block (least squares along the told direction)
    n = a.block
    num = den = 0
    used = 0
    for i in range(len(frames)):
        for j in range(i + 1, len(frames)):
            t = frames[j]["jitter"] - frames[i]["jitter"]
            if np.hypot(*t) < 0.3:
                continue
            A, B = frames[i]["y"], frames[j]["y"]
            g = (np.roll(A, -1, 1) - np.roll(A, 1, 1)) / 2 * t[0] + (np.roll(A, -1, 0) - np.roll(A, 1, 0)) / 2 * t[1]
            num = num + box(-(B - A) * g, n)
            den = den + box(g * g, n)
            used += 1
    if not used:
        sys.exit("no pair of frames differs enough in told jitter to measure")
    ratio = np.clip(num / np.maximum(den, 1e-9), -0.25, 1.25)
    detail = den / (n * n * used)
    ok = detail > 1.0
    bins = [-0.26, 0.2, 0.8, 1.26]
    hist = np.histogram(ratio[ok], bins)[0] / max(1, ok.sum())
    print("\n1. Does the input move with the told jitter?  (%d frame pairs, %.0f%% of the picture has enough detail to judge)" % (used, 100 * ok.mean()))
    print("   moved fully      (0.8 to 1.2): %5.1f%%" % (100 * hist[2]))
    print("   moved partly     (0.2 to 0.8): %5.1f%%" % (100 * hist[1]))
    print("   did not move  (-0.2 to 0.2):   %5.1f%%" % (100 * hist[0]))
    print("   median %.2f, mean %.2f of the told shift" % (np.median(ratio[ok]), ratio[ok].mean()))

    # 2. output steady while the input moves?  3. does the shown picture follow the output?
    outs, shown = [], None
    for f in frames:
        rows, fmt = plane(a.folder, f["index"], "dlssout", f["meta"])
        if rows is not None:
            rgb, hdr = colour(rows, fmt)
            if rgb is not None:
                outs.append((f, view_luma(rgb, hdr)))
    if len(outs) >= 2:
        din = np.mean([np.abs(view_luma(x[0]["rgb"], x[0]["hdr"]) - view_luma(y[0]["rgb"], y[0]["hdr"])).mean() for x, y in zip(outs, outs[1:])])
        dout = np.mean([np.abs(x[1] - y[1]).mean() for x, y in zip(outs, outs[1:])])
        print("\n2. Is DLAA's output steady while its input moves?")
        print("   frame-to-frame change: input %.2f, output %.2f (of 255)  ->  %s" % (
            din, dout, "steady: DLAA is gathering the frames" if dout < 0.5 * din else "NOT steady: DLAA is not gathering the frames (history lost every frame?)"))
        f, yo = outs[-1]
        yi = view_luma(f["rgb"], f["hdr"])
        lap = lambda y: 4 * y - np.roll(y, 1, 0) - np.roll(y, -1, 0) - np.roll(y, 1, 1) - np.roll(y, -1, 1)
        print("   fine detail (edge energy): input %.2f, output %.2f" % (np.abs(lap(yi)).mean(), np.abs(lap(yo)).mean()))
        rows, fmt = plane(a.folder, f["index"], "present", f["meta"])
        if rows is not None:
            rgb, hdr = colour(rows, fmt)
            if rgb is not None and rgb.shape == f["rgb"].shape:
                shown = view_luma(rgb, hdr)
                c = lambda p, q: float(np.corrcoef(lap(p)[8:-8, 8:-8].ravel(), lap(q)[8:-8, 8:-8].ravel())[0, 1])
                co, ci = c(shown, yo), c(shown, yi)
                print("\n3. Does the shown picture follow DLAA's output or its input?")
                print("   fine-detail match with the output %.3f, with the input %.3f  ->  %s" % (
                    co, ci, "the output reaches the screen" if co > ci + 0.02 else "the screen looks like the INPUT: the output is not what is shown" if ci > co + 0.02 else "no clear difference"))

    # 4. depth and motion
    f = frames[-1]
    rows, fmt = plane(a.folder, f["index"], "depth", f["meta"])
    if rows is not None and fmt == 41:
        d = rows.view(np.float32)
        print("\n4. Depth: min %.5f max %.5f, %.1f%% of pixels at the far/clear value" % (d.min(), d.max(), 100 * max((d == d.max()).mean(), (d == d.min()).mean())))
    rows, fmt = plane(a.folder, f["index"], "motion", f["meta"])
    if rows is not None and fmt == 16:
        m = rows.view(np.float32).reshape(rows.shape[0], -1, 2)
        mag = np.hypot(m[..., 0], m[..., 1])
        print("   Motion: mean %.3f px, largest %.2f px, %.1f%% of pixels above 0.05 px (camera %s)" % (
            mag.mean(), mag.max(), 100 * (mag > 0.05).mean(), "still" if f["still"] else "moving"))

    if a.map:
        from PIL import Image
        hh, ww = ratio.shape
        y = frames[-1]["y"][n // 2: n // 2 + hh, n // 2: n // 2 + ww]
        base = np.repeat(np.clip(y, 0, 255)[..., None], 3, 2)
        t = np.clip(ratio, 0, 1)[..., None]
        heat = np.concatenate([(1 - t) * 255, t * 255, np.zeros_like(t)], 2)
        alpha = np.clip(detail / 2.0, 0, 1)[..., None] * 0.65
        Image.fromarray((base * (1 - alpha) + heat * alpha).astype(np.uint8)).resize((ww // 2, hh // 2)).save(a.map)
        print("\nmap written: %s  (green moved with the jitter, red did not, grey has too little detail to judge)" % a.map)


if __name__ == "__main__":
    main()
