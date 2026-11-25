#!/usr/bin/env python3
"""Fold trial, part 2 (2026-11-25): a made-up scene with leaves, wire and brick, for the question
"does foliage disqualify the fold idea, or was the first trial's foliage result the trial's own limit?"

The first trial folded saved DLAA frames. Those frames only allow four jitter positions for a half-size
picture, and there was no ordinary DLSS Performance picture of the same scene to compare with. This
script draws its own scene, so both are available, and the exact answer (a supersampled picture) too.

  generate <out>   writes capture-format folders that tests/host/dlss_capture_replay replays unchanged:
      native        full size, DLAA jitter                         -> the DLAA reference
      half-true     an ordinary half-size render, full jitter      -> DLSS Performance as games do it
      half-4phase   the same with only four jitter positions       -> the jitter limit on its own
      fold-pick     coarse shading emulated, one fixed pixel kept  -> the fold idea as proposed
      fold-cycle    coarse shading, a different pixel kept in turn -> an adjustment
      fold-split    fold-pick, told jitter moved an eighth pixel   -> an adjustment
      repro-trial1  the native frames folded as the first trial did (four picks, DLAA's small jitter)
    and truth/NNNNN.npy: the exact picture (4x4 supersampled) for the last frames.
  evaluate <out> <replay out>   after the replays: numbers against the exact picture, and pictures.

Coarse shading is emulated to the Direct3D rule: coverage and depth are taken for every full-size pixel,
the colour (and an alpha test, which is part of the colour program) once per 2x2 block at the block's
centre. Thin wire and the wall's top edge are geometry; leaves are an alpha-tested card, as in games.
The scene pans steadily. Part of the foliage also sways, and that sway is NOT in the motion vectors,
which is how our program sees a game's wind-blown leaves.
"""
import json
import os
import sys

import numpy as np

W, H = 1600, 900          # full size (even)
FRAMES = 32
TRUTH_LAST = 8            # exact pictures for this many last frames
PAN = np.array([1.37, 0.23])  # full-size pixels a frame, for the nearest layer
PICKS = [(0, 0), (1, 1), (1, 0), (0, 1)]


def halton(i, b):
    f, r = 1.0, 0.0
    while i:
        f /= b
        r += f * (i % b)
        i //= b
    return r


def jitter(i, phases):
    k = (i % phases) + 1
    return halton(k, 2) - 0.5, halton(k, 3) - 0.5


def vnoise(x, y, seed):
    xi = np.floor(x)
    yi = np.floor(y)
    fx = x - xi
    fy = y - yi
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    xi = xi.astype(np.int64)
    yi = yi.astype(np.int64)

    def h(a, b):
        v = (a * 374761393 + b * 668265263 + seed * 2246822519) & 0xFFFFFFFF
        v = ((v ^ (v >> 13)) * 1274126177) & 0xFFFFFFFF
        v = v ^ (v >> 16)
        return v.astype(np.float64) / 4294967296.0

    a = h(xi, yi)
    b = h(xi + 1, yi)
    c = h(xi, yi + 1)
    d = h(xi + 1, yi + 1)
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy


def scene(gx, gy, sx, sy, t):
    """Colour (h, w, 3), depth, motion (full-size pixels, current to previous) for samples whose geometry is
    taken at (gx, gy) and whose colour program runs at (sx, sy). Full-size pixel coordinates."""
    cam = PAN * t
    par = [0.05, 0.30, 0.60, 1.00]
    shape = gx.shape
    col = np.zeros(shape + (3,))
    depth = np.zeros(shape)
    layer = np.zeros(shape, dtype=np.int32)

    # 0 sky: slow gradient and soft cloud.
    wx, wy = sx + cam[0] * par[0], sy + cam[1] * par[0]
    cloud = vnoise(wx / 90.0, wy / 50.0, 1) * 0.5 + vnoise(wx / 31.0, wy / 23.0, 2) * 0.25
    v = np.clip(sy / H, 0, 1)
    sky = np.stack([0.55 + 0.25 * cloud - 0.15 * v, 0.62 + 0.25 * cloud - 0.10 * v, 0.72 + 0.22 * cloud], -1)
    col[:] = sky
    depth[:] = 0.0005

    # 1 far lattice and wires: thin dark geometry (coverage at the geometry sample).
    wx, wy = gx + cam[0] * par[1], gy + cam[1] * par[1]
    u = (wx * 0.94 + wy * 0.34) % 47.0
    w2 = (wx * -0.5 + wy * 0.87) % 61.0
    lattice = ((np.minimum(u, 47.0 - u) < 0.65) | (np.minimum(w2, 61.0 - w2) < 0.65)) & (wx % 520.0 < 210.0) & (gy < H * 0.62)
    sag = H * 0.18 + 26.0 * np.sin(wx / 170.0) + wx * 0.02
    wire = np.zeros(shape, dtype=bool)
    for k in range(4):
        wire |= np.abs(wy - (sag + k * 21.0)) < 0.6
    barb = (np.abs(wy - sag) < 3.2) & ((wx % 13.0) < 1.1)
    m1 = lattice | wire | barb
    col[m1] = np.array([0.07, 0.07, 0.08])
    depth[m1] = 0.02
    layer[m1] = 1

    # 2 wall: top edge at a shallow angle (geometry), brick and grit (colour program).
    wxg, wyg = gx + cam[0] * par[2], gy + cam[1] * par[2]
    m2 = wyg > (H * 0.60 + 0.045 * wxg + 6.0 * np.sin(wxg / 140.0))
    wx, wy = sx + cam[0] * par[2], sy + cam[1] * par[2]
    row = np.floor(wy / 17.0)
    bx = (wx + (row % 2) * 21.0) % 42.0
    by = wy % 17.0
    mortar = (bx < 1.6) | (by < 1.6)
    grit = vnoise(wx / 1.7, wy / 1.7, 3) * 0.5 + vnoise(wx / 4.3, wy / 4.3, 4) * 0.5
    tone = vnoise(np.floor((wx + (row % 2) * 21.0) / 42.0), row, 5)
    brick = np.stack([0.42 + 0.22 * tone, 0.20 + 0.10 * tone, 0.14 + 0.06 * tone], -1) * (0.55 + 0.9 * grit)[..., None]
    brick[mortar] = np.array([0.50, 0.48, 0.44]) * (0.6 + 0.6 * grit[mortar])[..., None]
    col[m2] = brick[m2]
    depth[m2] = 0.08
    layer[m2] = 2

    # 3 foliage: an alpha-tested card over the right part and a band along the top. The alpha test is part
    #   of the colour program. Right of x = 0.82 W the leaves sway; the sway is not in the motion vectors.
    wx, wy = sx + cam[0] * par[3], sy + cam[1] * par[3]
    sway = (sx > W * 0.82)
    wx = wx + np.where(sway, 1.6 * np.sin(t * 0.55 + wy / 37.0), 0.0)
    wy = wy + np.where(sway, 0.9 * np.sin(t * 0.41 + wx / 53.0), 0.0)
    n = vnoise(wx / 19.0, wy / 19.0, 6) * 0.5 + vnoise(wx / 6.5, wy / 6.5, 7) * 0.32 + vnoise(wx / 2.6, wy / 2.6, 8) * 0.18
    region = np.maximum(np.clip((sx - W * 0.60) / (W * 0.10), 0, 1), np.clip((H * 0.13 - sy) / (H * 0.08), 0, 1))
    twig = (np.abs(((wx * 0.8 + wy * 0.6) % 83.0) - 41.0) < 0.7) | (np.abs(((wx * -0.3 + wy * 0.95) % 71.0) - 35.0) < 0.6)
    alpha = ((n > 0.86 - 0.36 * region) | (twig & (region > 0.25))) & (region > 0.02)
    shade = vnoise(wx / 3.1, wy / 3.1, 9)
    hue = vnoise(wx / 23.0, wy / 23.0, 10)
    leaf = np.stack([0.10 + 0.22 * hue, 0.26 + 0.34 * shade, 0.06 + 0.10 * hue], -1) * (0.45 + 0.9 * shade)[..., None]
    leaf[twig] = np.array([0.16, 0.11, 0.07])
    col[alpha] = leaf[alpha]
    depth[alpha] = 0.30
    layer[alpha] = 3

    mv = np.zeros(shape + (2,))
    for k in range(4):
        mv[layer == k] = PAN * par[k]
    return col, depth, mv


def grid(w, h):
    y, x = np.mgrid[0:h, 0:w]
    return x.astype(np.float64), y.astype(np.float64)


def write_frame(folder, index, col, depth, mv, jit, quality, out_wh):
    h, w = depth.shape
    rgba = np.concatenate([col, np.ones((h, w, 1))], -1).astype(np.float16)
    rgba.tofile(os.path.join(folder, "%05d-color.raw" % index))
    depth.astype(np.float32).tofile(os.path.join(folder, "%05d-depth.raw" % index))
    mv.astype(np.float32).tofile(os.path.join(folder, "%05d-motion.raw" % index))
    write_meta(folder, index, w, h, jit, quality, out_wh)


def write_meta(folder, index, w, h, jit, quality, out_wh):
    meta = {
        "capture_kind": "evaluation", "evaluation_succeeded": True, "present_ordinal": 1000 + index,
        "size": [w, h], "jitter_px": [float(jit[0]), float(jit[1])], "mv_scale": [1.0, 1.0],
        "preset": 13, "debug_view": 0, "quality": quality, "pre_exposure": 1.0, "exposure_scale": 1.0,
        "hdr": True, "depth_inverted": True, "auto_exposure": True, "mv_lowres": True, "mv_jittered": False,
        "planes": [
            {"name": "color", "width": w, "height": h, "dxgi_format": 10, "row_pitch": w * 8},
            {"name": "depth", "width": w, "height": h, "dxgi_format": 41, "row_pitch": w * 4},
            {"name": "motion", "width": w, "height": h, "dxgi_format": 16, "row_pitch": w * 8},
            {"name": "dlssout", "width": out_wh[0], "height": out_wh[1], "dxgi_format": 10, "row_pitch": out_wh[0] * 8},
        ],
    }
    with open(os.path.join(folder, "%05d-meta.json" % index), "w") as f:
        f.write(json.dumps(meta))


VARIANTS = ["native", "half-true", "half-4phase", "fold-pick", "fold-cycle", "fold-split", "repro-trial1"]


def generate(out, frames=FRAMES, size=None):
    global W, H
    if size:
        W, H = size
    hw, hh = W // 2, H // 2
    for v in VARIANTS + ["truth"]:
        os.makedirs(os.path.join(out, v), exist_ok=True)
    X, Y = grid(W, H)
    hx, hy = grid(hw, hh)
    for t in range(frames):
        # native: sample = pixel centre - jitter.
        jn = jitter(t, 16)
        col, dep, mv = scene(X + 0.5 - jn[0], Y + 0.5 - jn[1], X + 0.5 - jn[0], Y + 0.5 - jn[1], t)
        write_frame(os.path.join(out, "native"), t, col, dep, mv, jn, 5, (W, H))

        # the first trial's fold of those native frames: four picks in turn, DLAA's small jitter.
        px, py = PICKS[t % 4]
        told = (0.25 - 0.5 * px + 0.5 * jn[0], 0.25 - 0.5 * py + 0.5 * jn[1])
        write_frame(os.path.join(out, "repro-trial1"), t, col[py::2, px::2], dep[py::2, px::2], mv[py::2, px::2] * 0.5, told, 0, (W, H))

        # an ordinary half-size render: half-size pixel centre - jitter, in full-size coordinates.
        for name, phases in (("half-true", 64), ("half-4phase", 4)):
            jh = jitter(t, phases)
            sx, sy = 2.0 * (hx + 0.5 - jh[0]), 2.0 * (hy + 0.5 - jh[1])
            col, dep, mv = scene(sx, sy, sx, sy, t)
            write_frame(os.path.join(out, name), t, col, dep, mv * 0.5, jh, 0, (W, H))

        # coarse shading on a full-size picture whose jitter is twice as wide: colour at the block centre,
        # geometry at the kept pixel's centre.
        jh = jitter(t, 64)
        sx, sy = 2.0 * (hx + 0.5 - jh[0]), 2.0 * (hy + 0.5 - jh[1])
        col, dep, mv = scene(sx + 0.5, sy + 0.5, sx, sy, t)  # kept pixel (1, 1)
        write_frame(os.path.join(out, "fold-pick"), t, col, dep, mv * 0.5, jh, 0, (W, H))
        write_frame(os.path.join(out, "fold-split"), t, col, dep, mv * 0.5, (jh[0] - 0.125, jh[1] - 0.125), 0, (W, H))
        px, py = PICKS[t % 4]
        col, dep, mv = scene(sx - 0.5 + px, sy - 0.5 + py, sx, sy, t)
        write_frame(os.path.join(out, "fold-cycle"), t, col, dep, mv * 0.5, jh, 0, (W, H))

        if t >= frames - TRUTH_LAST:
            acc = np.zeros((H, W, 3))
            for a in range(4):
                for b in range(4):
                    ox, oy = (a + 0.5) / 4.0, (b + 0.5) / 4.0
                    c, _, _ = scene(X + ox, Y + oy, X + ox, Y + oy, t)
                    acc += c
            np.save(os.path.join(out, "truth", "%05d.npy" % t), (acc / 16.0).astype(np.float32))
        print("frame", t, flush=True)
    with open(os.path.join(out, "scene.json"), "w") as f:
        json.dump({"width": W, "height": H, "frames": frames, "truth_last": TRUTH_LAST, "variants": VARIANTS}, f)


def display(x):
    x = np.nan_to_num(np.maximum(x.astype(np.float64), 0.0))
    x = x / (1.0 + x)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055) * 255.0


def luma(d):
    return 0.2126 * d[..., 0] + 0.7152 * d[..., 1] + 0.0722 * d[..., 2]


def regions(w, h):
    """Name -> (x, y, w, h), 320x200 each, placed on the scene's parts."""
    return {
        "A-wire": (int(w * 0.20), int(h * 0.17), 320, 200),
        "B-brick": (int(w * 0.18), int(h * 0.74), 320, 200),
        "C-wall-edge": (int(w * 0.30), int(h * 0.57), 320, 200),
        "D-leaves-still": (int(w * 0.66) - 160 + 80, int(h * 0.36), 320, 200),
        "E-leaves-swaying": (int(w * 0.84), int(h * 0.36), 320 if w * 0.84 + 320 <= w else w - int(w * 0.84), 200),
    }


def evaluate(out, replay):
    from PIL import Image, ImageDraw
    info = json.load(open(os.path.join(out, "scene.json")))
    w, h, frames = info["width"], info["height"], info["frames"]
    last = min(info["truth_last"], frames)
    reg = regions(w, h)
    pics = os.path.join(replay, "compare")
    os.makedirs(pics, exist_ok=True)
    lines = []
    labels = {"native": "DLAA today", "half-true": "Normal DLSS Performance", "half-4phase": "Performance, 4 jitter spots",
              "fold-pick": "Fold (as proposed)", "fold-cycle": "Fold, pixel in turn", "fold-split": "Fold, jitter nudged",
              "repro-trial1": "First trial's method"}
    err = {v: {r: [] for r in list(reg) + ["whole"]} for v in info["variants"]}
    final = {}
    for t in range(frames - last, frames):
        truth = luma(display(np.load(os.path.join(out, "truth", "%05d.npy" % t))))
        if t == frames - 1:
            final["truth"] = display(np.load(os.path.join(out, "truth", "%05d.npy" % t)))
        for v in info["variants"]:
            raw = np.fromfile(os.path.join(replay, v, "%05d-dlssout.raw" % t), dtype=np.float16).reshape(h, w, 4)[..., :3]
            d = display(raw)
            if t == frames - 1:
                final[v] = d
            diff = np.abs(luma(d) - truth)
            err[v]["whole"].append(diff.mean())
            for r, (x, y, rw, rh) in reg.items():
                err[v][r].append(diff[y:y + rh, x:x + rw].mean())
    lines.append("Mean absolute brightness error against the exact picture (0..255 display scale), last %d frames." % last)
    lines.append("Lower is better. 'wobble' = how much that error varies from frame to frame (standard deviation).")
    cols = ["whole"] + list(reg)
    lines.append("%-30s" % "method" + "".join("%20s" % c for c in cols))
    for v in info["variants"]:
        lines.append("%-30s" % labels[v] + "".join("%20s" % ("%.2f (wobble %.2f)" % (np.mean(err[v][c]), np.std(err[v][c]))) for c in cols))
    open(os.path.join(pics, "numbers.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    order = ["truth", "native", "half-true", "fold-pick", "fold-cycle", "fold-split", "half-4phase", "repro-trial1"]
    names = dict(labels, truth="Exact picture")
    for r, (x, y, rw, rh) in reg.items():
        sheet = Image.new("RGB", ((rw * 2 + 8) * 4, (rh * 2 + 30) * 2), (24, 24, 24))
        dr = ImageDraw.Draw(sheet)
        for k, v in enumerate(order):
            crop = Image.fromarray(np.clip(final[v][y:y + rh, x:x + rw], 0, 255).astype(np.uint8)).resize((rw * 2, rh * 2), Image.NEAREST)
            ox, oy = (k % 4) * (rw * 2 + 8), (k // 4) * (rh * 2 + 30)
            dr.text((ox + 4, oy + 8), names[v], fill=(255, 255, 255))
            sheet.paste(crop, (ox, oy + 30))
        sheet.save(os.path.join(pics, "compare-%s.jpg" % r), quality=92)
    over = Image.fromarray(np.clip(final["fold-pick"], 0, 255).astype(np.uint8))
    dr = ImageDraw.Draw(over)
    for r, (x, y, rw, rh) in reg.items():
        dr.rectangle([x, y, x + rw, y + rh], outline=(255, 230, 0), width=3)
        dr.text((x + 6, y + 6), r, fill=(255, 230, 0))
    over.save(os.path.join(pics, "overview.jpg"), quality=90)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "generate":
        generate(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else FRAMES,
                 (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) > 5 else None)
    elif len(sys.argv) >= 4 and sys.argv[1] == "evaluate":
        evaluate(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(2)
