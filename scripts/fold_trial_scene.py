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


# None: textures read at the exact point (no mips). A number: textures read as mip-mapped ones with this
# bias for the half-size and folded pictures (NVIDIA's Performance value is -2); the full-size DLAA
# picture then uses -1, its own value.
MIP_BIAS = None
USE_R2 = False  # True: Roberts' R2 jitter instead of Halton (Cort's preference, 2026-11-25)


def jitter(i, phases):
    if USE_R2:
        return jitter_r2(i, phases)
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


# Depth (reversed: larger is nearer) is this times a layer's share of the pan, as a sideways camera
# movement gives it: the supplied motion is then exactly "depth times a shear", which our flow's
# camera path can be given as a camera (tests/host/flow_motion_dump --mode camera).
DEPTH_PER_PAR = 0.30
# Trial 4: the band of leaves along the top (left of 0.55 W) sways this many times harder than the
# leaves on the right, like a gust: up to about 3.5 full-size pixels a frame against 0.9. 0 = that band is still.
GUST = 0.0
LEAF_MOTION = False  # True: the swaying leaves' own motion is in the motion vectors (as optical flow would supply)


def scene(gx, gy, sx, sy, t, foot=0.0, parts=False):
    """Colour (h, w, 3), depth, motion (full-size pixels, current to previous) for samples whose geometry is
    taken at (gx, gy) and whose colour program runs at (sx, sy). Full-size pixel coordinates.
    foot > 0: the colour program's textures (brick, leaves and the leaves' cut-out) are read as a game's
    mip-mapped textures are, averaged over a square of `foot` full-size pixels, the cut-out tested after
    the averaging. foot = 0 reads them at the exact point (no mip filtering at all)."""
    if foot > 0.0:
        acc = None
        for ox, oy in ((-0.25, -0.25), (0.25, -0.25), (-0.25, 0.25), (0.25, 0.25)):
            p = scene(gx, gy, sx + ox * foot, sy + oy * foot, t, 0.0, True)
            acc = p if acc is None else [a + b for a, b in zip(acc, p)]
        col, leaf, alpha, depth, layer, sdx, sdy = [a / 4.0 for a in acc]
        return _finish(col, leaf, alpha >= 0.5, depth, np.rint(layer).astype(np.int32), sdx, sdy)
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
    depth[:] = DEPTH_PER_PAR * par[0]

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
    depth[m1] = DEPTH_PER_PAR * par[1]
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
    depth[m2] = DEPTH_PER_PAR * par[2]
    layer[m2] = 2

    # 3 foliage: an alpha-tested card over the right part and a band along the top. The alpha test is part
    #   of the colour program. Right of x = 0.82 W the leaves sway; the sway is not in the motion vectors.
    wx, wy = sx + cam[0] * par[3], sy + cam[1] * par[3]
    sway = np.where(sx > W * 0.82, 1.0, np.where((sy < H * 0.13) & (sx < W * 0.55), GUST, 0.0))
    dx = sway * 1.6 * np.sin(t * 0.55 + wy / 37.0)
    dx_before = sway * 1.6 * np.sin((t - 1) * 0.55 + wy / 37.0)
    wx = wx + dx
    dy = sway * 0.9 * np.sin(t * 0.41 + wx / 53.0)
    dy_before = sway * 0.9 * np.sin((t - 1) * 0.41 + wx / 53.0)
    wy = wy + dy
    n = vnoise(wx / 19.0, wy / 19.0, 6) * 0.5 + vnoise(wx / 6.5, wy / 6.5, 7) * 0.32 + vnoise(wx / 2.6, wy / 2.6, 8) * 0.18
    region = np.maximum(np.clip((sx - W * 0.60) / (W * 0.10), 0, 1), np.clip((H * 0.13 - sy) / (H * 0.08), 0, 1))
    twig = (np.abs(((wx * 0.8 + wy * 0.6) % 83.0) - 41.0) < 0.7) | (np.abs(((wx * -0.3 + wy * 0.95) % 71.0) - 35.0) < 0.6)
    alpha = ((n > 0.86 - 0.36 * region) | (twig & (region > 0.25))) & (region > 0.02)
    shade = vnoise(wx / 3.1, wy / 3.1, 9)
    hue = vnoise(wx / 23.0, wy / 23.0, 10)
    leaf = np.stack([0.10 + 0.22 * hue, 0.26 + 0.34 * shade, 0.06 + 0.10 * hue], -1) * (0.45 + 0.9 * shade)[..., None]
    leaf[twig] = np.array([0.16, 0.11, 0.07])
    if parts:
        return [col, leaf, alpha.astype(np.float64), depth, layer.astype(np.float64), dx - dx_before, dy - dy_before]
    return _finish(col, leaf, alpha, depth, layer, dx - dx_before, dy - dy_before)


def _finish(col, leaf, alpha, depth, layer, sdx, sdy):
    """The leaves over what is behind them, and the motion of the result."""
    par = [0.05, 0.30, 0.60, 1.00]
    col = col.copy()
    depth = depth.copy()
    layer = layer.copy()
    col[alpha] = leaf[alpha]
    depth[alpha] = DEPTH_PER_PAR * par[3]
    layer[alpha] = 3

    mv = np.zeros(depth.shape + (2,))
    for k in range(4):
        mv[layer == k] = PAN * par[k]
    # A leaf point sits at screen = world - camera - sway, so previous - current = pan + sway now - sway before.
    exact = mv.copy()
    exact[..., 0] += np.where(layer == 3, sdx, 0.0)
    exact[..., 1] += np.where(layer == 3, sdy, 0.0)
    if LEAF_MOTION:
        mv = exact
    return col, depth, mv, exact


def grid(w, h):
    y, x = np.mgrid[0:h, 0:w]
    return x.astype(np.float64), y.astype(np.float64)


def write_frame(folder, index, col, depth, mv, jit, quality, out_wh, exact=None):
    if not os.path.isdir(folder):
        return  # a variant this run does not make
    side = os.path.join(os.path.dirname(folder), "exact", os.path.basename(folder))
    if exact is not None and os.path.isdir(side):
        exact.astype(np.float32).tofile(os.path.join(side, "%05d-motion.raw" % index))
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
        # the supplied motion as a camera: pixels (of this picture) per unit of depth
        "trial_shear_px": [float(PAN[0] / DEPTH_PER_PAR * w / W), float(PAN[1] / DEPTH_PER_PAR * w / W)],
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


def generate(out, frames=FRAMES, size=None, only=None, exact=False):
    global W, H
    keep = only or VARIANTS
    if size:
        W, H = size
    hw, hh = W // 2, H // 2
    for v in list(keep) + ["truth"]:
        os.makedirs(os.path.join(out, v), exist_ok=True)
    if exact:  # the motion with the leaves' sway in it, beside each variant (replay --motion-dir)
        for v in keep:
            os.makedirs(os.path.join(out, "exact", v), exist_ok=True)
    X, Y = grid(W, H)
    hx, hy = grid(hw, hh)
    for t in range(frames):
        # native: sample = pixel centre - jitter.
        jn = jitter(t, 16)
        foot_n = 0.0 if MIP_BIAS is None else 0.5
        foot_h = 0.0 if MIP_BIAS is None else 2.0 * 2.0 ** MIP_BIAS
        col, dep, mv, ex = scene(X + 0.5 - jn[0], Y + 0.5 - jn[1], X + 0.5 - jn[0], Y + 0.5 - jn[1], t, foot_n)
        write_frame(os.path.join(out, "native"), t, col, dep, mv, jn, 5, (W, H), ex)

        # the first trial's fold of those native frames: four picks in turn, DLAA's small jitter.
        px, py = PICKS[t % 4]
        told = (0.25 - 0.5 * px + 0.5 * jn[0], 0.25 - 0.5 * py + 0.5 * jn[1])
        write_frame(os.path.join(out, "repro-trial1"), t, col[py::2, px::2], dep[py::2, px::2], mv[py::2, px::2] * 0.5, told, 0, (W, H))

        # an ordinary half-size render: half-size pixel centre - jitter, in full-size coordinates.
        for name, phases in (("half-true", 64), ("half-4phase", 4)):
            jh = jitter(t, phases)
            sx, sy = 2.0 * (hx + 0.5 - jh[0]), 2.0 * (hy + 0.5 - jh[1])
            col, dep, mv, ex = scene(sx, sy, sx, sy, t, foot_h)
            write_frame(os.path.join(out, name), t, col, dep, mv * 0.5, jh, 0, (W, H), ex * 0.5)

        # coarse shading on a full-size picture whose jitter is twice as wide: colour at the block centre,
        # geometry at the kept pixel's centre.
        jh = jitter(t, 64)
        sx, sy = 2.0 * (hx + 0.5 - jh[0]), 2.0 * (hy + 0.5 - jh[1])
        col, dep, mv, ex = scene(sx + 0.5, sy + 0.5, sx, sy, t, foot_h)  # kept pixel (1, 1)
        write_frame(os.path.join(out, "fold-pick"), t, col, dep, mv * 0.5, jh, 0, (W, H))
        write_frame(os.path.join(out, "fold-split"), t, col, dep, mv * 0.5, (jh[0] - 0.125, jh[1] - 0.125), 0, (W, H))
        px, py = PICKS[t % 4]
        col, dep, mv, ex = scene(sx - 0.5 + px, sy - 0.5 + py, sx, sy, t, foot_h)
        write_frame(os.path.join(out, "fold-cycle"), t, col, dep, mv * 0.5, jh, 0, (W, H), ex * 0.5)

        if t >= frames - TRUTH_LAST:
            acc = np.zeros((H, W, 3))
            for a in range(4):
                for b in range(4):
                    ox, oy = (a + 0.5) / 4.0, (b + 0.5) / 4.0
                    c = scene(X + ox, Y + oy, X + ox, Y + oy, t)[0]
                    acc += c
            np.save(os.path.join(out, "truth", "%05d.npy" % t), (acc / 16.0).astype(np.float32))
        print("frame", t, flush=True)
    with open(os.path.join(out, "scene.json"), "w") as f:
        json.dump({"width": W, "height": H, "frames": frames, "truth_last": TRUTH_LAST, "variants": list(keep), "leaf_motion": LEAF_MOTION, "gust": GUST,
                   "mip_bias": MIP_BIAS, "jitter": "R2" if USE_R2 else "Halton"}, f)


MIP_VARIANTS = ["native-b1", "half-b0", "half-b1", "half-b2", "half-b225", "half-b2-halton", "fold-b0", "fold-b2", "fold-b225"]


def jitter_r2(i, phases):
    """Martin Roberts' R2 sequence, as src/core/temporal_jitter.hpp temporal_jitter_r2 gives it."""
    n = float(i % phases + 1)
    return (0.5 + n * 0.7548776662466927) % 1.0 - 0.5, (0.5 + n * 0.5698402909980532) % 1.0 - 0.5


def generate_mip(out, frames=FRAMES, size=None):
    """Trial 5 (Cort, 2026-11-25: "include the required mip bias"; R2 jitter preferred): the scene's
    textures read as mip-mapped ones. NVIDIA's rule is bias = log2(render / display) - 1: DLAA -1,
    Performance -2 (our program uses -2.25 there). A picture rendered at share s of full size with bias b
    reads its textures over 2^b / s full-size pixels: DLAA at -1 over half a pixel, half size with no bias
    over two, half size at -2 over half a pixel again. Coarse shading picks its mip like a half-size
    render. The leaves' exact motion is supplied, so only the texture reading differs."""
    global W, H, LEAF_MOTION
    LEAF_MOTION = True
    if size:
        W, H = size
    hw, hh = W // 2, H // 2
    for v in MIP_VARIANTS + ["truth"]:
        os.makedirs(os.path.join(out, v), exist_ok=True)
    X, Y = grid(W, H)
    hx, hy = grid(hw, hh)
    foot = {"b0": 2.0, "b1": 1.0, "b2": 0.5, "b225": 2.0 * 2.0 ** -2.25}
    for t in range(frames):
        jn = jitter_r2(t, 16)
        col, dep, mv, _ = scene(X + 0.5 - jn[0], Y + 0.5 - jn[1], X + 0.5 - jn[0], Y + 0.5 - jn[1], t, 0.5)
        write_frame(os.path.join(out, "native-b1"), t, col, dep, mv, jn, 5, (W, H))
        jh = jitter_r2(t, 64)
        sx, sy = 2.0 * (hx + 0.5 - jh[0]), 2.0 * (hy + 0.5 - jh[1])
        for b in ("b0", "b1", "b2", "b225"):
            col, dep, mv, _ = scene(sx, sy, sx, sy, t, foot[b])
            write_frame(os.path.join(out, "half-" + b), t, col, dep, mv * 0.5, jh, 0, (W, H))
        px, py = PICKS[t % 4]
        for b in ("b0", "b2", "b225"):
            col, dep, mv, _ = scene(sx - 0.5 + px, sy - 0.5 + py, sx, sy, t, foot[b])
            write_frame(os.path.join(out, "fold-" + b), t, col, dep, mv * 0.5, jh, 0, (W, H))
        jo = jitter(t, 64)  # Halton, for the comparison with R2
        ox_, oy_ = 2.0 * (hx + 0.5 - jo[0]), 2.0 * (hy + 0.5 - jo[1])
        col, dep, mv, _ = scene(ox_, oy_, ox_, oy_, t, foot["b2"])
        write_frame(os.path.join(out, "half-b2-halton"), t, col, dep, mv * 0.5, jo, 0, (W, H))
        if t >= frames - TRUTH_LAST:
            acc = np.zeros((H, W, 3))
            for a in range(4):
                for b in range(4):
                    ox, oy = (a + 0.5) / 4.0, (b + 0.5) / 4.0
                    acc += scene(X + ox, Y + oy, X + ox, Y + oy, t)[0]
            np.save(os.path.join(out, "truth", "%05d.npy" % t), (acc / 16.0).astype(np.float32))
        print("frame", t, flush=True)
    with open(os.path.join(out, "scene.json"), "w") as f:
        json.dump({"width": W, "height": H, "frames": frames, "truth_last": TRUTH_LAST, "variants": MIP_VARIANTS, "leaf_motion": True, "gust": GUST}, f)


def display(x):
    x = np.nan_to_num(np.maximum(x.astype(np.float64), 0.0))
    x = x / (1.0 + x)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055) * 255.0


def luma(d):
    return 0.2126 * d[..., 0] + 0.7152 * d[..., 1] + 0.0722 * d[..., 2]


WHOLE_ROWS = 0.80  # the "whole" number leaves out the bottom fifth (the DLSS indicator's text, when it is on)


def regions(w, h, gust=False):
    """Name -> (x, y, w, h), 320x200 each, placed on the scene's parts."""
    r = regions_base(w, h)
    if gust:
        r["F-leaves-gusting"] = (int(w * 0.08), 0, 320, int(h * 0.11))
    return r


def regions_base(w, h):
    return {
        "A-wire": (int(w * 0.20), int(h * 0.17), 320, 200),
        # Above the bottom fifth: with NVIDIA's DLSS indicator switched on (as on Cort's PC) its text is
        # burned into the bottom-left of every DLSS output, and until 2026-11-25 this region sat under it.
        "B-brick": (int(w * 0.18), int(h * 0.655), 320, int(h * 0.14)),
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
              "repro-trial1": "First trial's method",
              "native-b1": "DLAA, bias -1 (R2)", "half-b0": "Performance, no bias", "half-b1": "Performance, bias -1",
              "half-b2": "Performance, bias -2", "half-b225": "Performance, bias -2.25", "half-b2-halton": "Perf., bias -2, Halton",
              "fold-b0": "Fold, no bias", "fold-b2": "Fold, bias -2", "fold-b225": "Fold, bias -2.25"}
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
            err[v]["whole"].append(diff[:int(h * WHOLE_ROWS)].mean())
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
    order = [v for v in ["truth", "native", "half-true"] + MIP_VARIANTS + ["fold-pick", "fold-cycle", "fold-split", "half-4phase", "repro-trial1"] if v in final]
    names = dict(labels, truth="Exact picture")
    for r, (x, y, rw, rh) in reg.items():
        sheet = Image.new("RGB", ((rw * 2 + 8) * 4, (rh * 2 + 30) * ((len(order) + 3) // 4)), (24, 24, 24))
        dr = ImageDraw.Draw(sheet)
        for k, v in enumerate(order):
            crop = Image.fromarray(np.clip(final[v][y:y + rh, x:x + rw], 0, 255).astype(np.uint8)).resize((rw * 2, rh * 2), Image.NEAREST)
            ox, oy = (k % 4) * (rw * 2 + 8), (k // 4) * (rh * 2 + 30)
            dr.text((ox + 4, oy + 8), names[v], fill=(255, 255, 255))
            sheet.paste(crop, (ox, oy + 30))
        sheet.save(os.path.join(pics, "compare-%s.jpg" % r), quality=92)
    over = Image.fromarray(np.clip(final["fold-cycle" if "fold-cycle" in final else order[-1]], 0, 255).astype(np.uint8))
    dr = ImageDraw.Draw(over)
    for r, (x, y, rw, rh) in reg.items():
        dr.rectangle([x, y, x + rw, y + rh], outline=(255, 230, 0), width=3)
        dr.text((x + 6, y + 6), r, fill=(255, 230, 0))
    over.save(os.path.join(pics, "overview.jpg"), quality=90)


# ---- Fold trial 4 (2026-11-25): our own optical flow on this scene -------------------------------
# Cort: "test a few things. With optical flow alone at strict rules, 50% looser, and 100% looser.
# Whichever is best, then run a test combining that with the other supplied motion."
# The flow is the program's own (tests/host/flow_motion_dump runs compute_flow and the arbiter on the
# scene's pictures); every motion field then goes through DLSS with the replay tool and is measured
# against the exact picture.
FLOW_VARIANTS = ["native", "half-true", "fold-cycle"]
FLOW_VARIANT_NAMES = {"native": "DLAA", "half-true": "Performance", "fold-cycle": "Fold"}
FLOW_DUMPS = [  # name, --mode, --loosen
    ("alone-0", "none", 0), ("alone-50", "none", 50), ("alone-100", "none", 100), ("total", "total", 0),
    ("correct-0", "camera", 0), ("correct-50", "camera", 50), ("correct-100", "camera", 100),
]
FLOW_LABELS = {
    "supplied": "Supplied only (no leaf motion)",
    "alone-0": "Flow alone, strict",
    "alone-50": "Flow alone, 50% looser",
    "alone-100": "Flow alone, 100% looser",
    "total": "Flow alone, no-camera measuring",
    "correct-0": "Supplied first, flow corrects, strict",
    "correct-50": "Supplied first, flow corrects, 50%",
    "correct-100": "Supplied first, flow corrects, 100%",
    "average": "Average of supplied and flow",
    "flowfirst": "Flow first, supplied elsewhere",
    "block-oracle": "Flow only on moving things (perfect block)",
    "block-oracle-alone": "Perfect block, flow alone inside",
    "block-detect": "Flow only where pictures disagree (real block)",
    "lean-oracle": "Supplied + lean on this frame (perfect zone)",
    "lean-detect": "Supplied + lean on this frame (real zone)",
    "lean-block-oracle": "Perfect block + lean on this frame",
    "exact": "Exact leaf motion (best possible)",
}
BLOCK_NAMES = ["block-oracle", "block-oracle-alone", "block-detect", "lean-oracle", "lean-detect", "lean-block-oracle"]
DETECT_THRESHOLD = 0.028  # mean brightness difference (0..1) after lining two frames up with the supplied motion


def _bilinear(img, x, y):
    h, w = img.shape
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx, fy = x - x0, y - y0
    return (img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx) * (1 - fy) + (img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx) * fy


def _box(a, r):
    k = 2 * r + 1
    c = np.cumsum(np.cumsum(np.pad(a, r, mode="edge"), 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def _frame_luma(folder, t, w, h):
    c = np.fromfile(os.path.join(folder, "%05d-color.raw" % t), dtype=np.float16).reshape(h, w, 4)[..., :3]
    return luma(display(c)) / 255.0


def _disagreement(folder, t, w, h):
    """How different this frame is from the one before after lining them up with the supplied motion
    (both frames' jitter taken out), averaged over a 7x7 window. Our own data only."""
    cur, prev = _frame_luma(folder, t, w, h), _frame_luma(folder, t - 1, w, h)
    mv = np.fromfile(os.path.join(folder, "%05d-motion.raw" % t), dtype=np.float32).reshape(h, w, 2).astype(np.float64)
    j = json.load(open(os.path.join(folder, "%05d-meta.json" % t)))["jitter_px"]
    jp = json.load(open(os.path.join(folder, "%05d-meta.json" % (t - 1))))["jitter_px"]
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    return _box(np.abs(cur - _bilinear(prev, x + mv[..., 0] + jp[0] - j[0], y + mv[..., 1] + jp[1] - j[1])), 3)

FLOW_ORDER = ["supplied", "alone-0", "alone-50", "alone-100", "total", "correct-0", "correct-50", "correct-100",
              "average", "flowfirst"] + BLOCK_NAMES + ["exact"]


def safe_cores():
    """Cort's PC: keep this process, and everything it starts, on the cores of mask 00FF33CC."""
    if os.name != "nt":
        return
    import ctypes
    k = ctypes.windll.kernel32
    k.GetCurrentProcess.restype = ctypes.c_void_p
    k.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    ok = k.SetProcessAffinityMask(k.GetCurrentProcess(), 0x00FF33CC)
    print("processor mask 00FF33CC:", "set" if ok else "NOT SET (this machine has other processors?)", flush=True)


def flow_run(cmd, log):
    import subprocess
    with open(log, "w") as f:
        f.write(" ".join(cmd) + "\n")
        f.flush()
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    return code


def flow_planes(folder, t, w, h):
    mv = np.fromfile(os.path.join(folder, "%05d-motion.raw" % t), dtype=np.float32).reshape(h, w, 2)
    path = os.path.join(folder, "%05d-prov.raw" % t)
    prov = np.fromfile(path, dtype=np.uint8).reshape(h, w) & 7 if os.path.isfile(path) else None
    return mv, prov


def flowtrial(scene_dir, work, bindir):
    """Everything after the scene exists: flow dumps, combinations, replays, numbers, pictures.
    Finished steps are skipped when it is started again. Nothing is deleted."""
    from PIL import Image, ImageDraw
    info = json.load(open(os.path.join(scene_dir, "scene.json")))
    W_, H_, frames = info["width"], info["height"], info["frames"]
    last = min(info["truth_last"], frames)
    dump_exe = os.path.join(bindir, "flow_motion_dump.exe")
    replay_exe = os.path.join(bindir, "dlss_capture_replay.exe")
    for exe in (dump_exe, replay_exe):
        if not os.path.isfile(exe):
            print("FATAL: missing", exe)
            return 1
    logs = os.path.join(work, "logs")
    os.makedirs(logs, exist_ok=True)
    notes = []

    def note(text):
        notes.append(text)
        print(text, flush=True)

    def size_of(v):
        return (W_, H_) if v == "native" else (W_ // 2, H_ // 2)

    def dump_dir(v, name):
        return os.path.join(work, "flow", v, name)

    def bias_dir(v, name):
        if name in ("lean-oracle", "lean-block-oracle"):
            return os.path.join(work, "bias", v, "oracle")
        if name == "lean-detect":
            return os.path.join(work, "bias", v, "detect")
        return None

    def motion_dir(v, name):
        if name in ("supplied", "lean-oracle", "lean-detect"):
            return None
        if name == "lean-block-oracle":
            return dump_dir(v, "block-oracle")
        if name == "exact":
            return os.path.join(scene_dir, "exact", v)
        return dump_dir(v, name)

    def do_dump(v, name, mode, loosen):
        out = dump_dir(v, name)
        if os.path.isfile(os.path.join(out, "%05d-motion.raw" % (frames - 1))):
            return True
        os.makedirs(os.path.dirname(out), exist_ok=True)
        code = flow_run([dump_exe, os.path.join(scene_dir, v), out, "--mode", mode, "--loosen", str(loosen)],
                        os.path.join(logs, "flow-%s-%s.txt" % (v, name)))
        if code != 0:
            note("FAILED (exit %d): flow %s %s; see logs" % (code, v, name))
        return code == 0

    def do_replay(v, name):
        root = os.path.join(work, "replay", v)
        if os.path.isfile(os.path.join(root, name, "%05d-dlssout.raw" % (frames - 1))):
            return True
        os.makedirs(root, exist_ok=True)
        cmd = [replay_exe, os.path.join(scene_dir, v), root, "--name", name, "--preroll", "0", "--save-last", str(last)]
        md = motion_dir(v, name)
        if md:
            if not os.path.isfile(os.path.join(md, "%05d-motion.raw" % (frames - 1))):
                return False
            cmd += ["--motion-dir", md]
        bd = bias_dir(v, name)
        if bd:
            if not os.path.isfile(os.path.join(bd, "%05d-bias.raw" % (frames - 1))):
                return False
            cmd += ["--bias-dir", bd]
        code = flow_run(cmd, os.path.join(logs, "replay-%s-%s.txt" % (v, name)))
        if code != 0:
            note("FAILED (exit %d): replay %s %s; see logs" % (code, v, name))
        return code == 0

    truth = {t: display(np.load(os.path.join(scene_dir, "truth", "%05d.npy" % t))) for t in range(frames - last, frames)}
    truth_luma = {t: luma(d) for t, d in truth.items()}
    reg = regions(W_, H_, info.get("gust", 0) > 0)
    cols = ["whole"] + list(reg)
    err, final = {}, {}
    shown = [c for c in ["F-leaves-gusting", "E-leaves-swaying", "D-leaves-still", "A-wire", "C-wall-edge", "B-brick", "whole"] if c in cols]

    def measure(v, name):
        """Picture error by region for one replay, or None."""
        if (v, name) in err:
            return err[(v, name)]
        folder = os.path.join(work, "replay", v, name)
        if not os.path.isfile(os.path.join(folder, "%05d-dlssout.raw" % (frames - 1))):
            return None
        e = {c: [] for c in cols}
        for t in range(frames - last, frames):
            d = display(np.fromfile(os.path.join(folder, "%05d-dlssout.raw" % t), dtype=np.float16).reshape(H_, W_, 4)[..., :3])
            if t == frames - 1:
                final[(v, name)] = d
            diff = np.abs(luma(d) - truth_luma[t])
            e["whole"].append(diff[:int(H_ * WHOLE_ROWS)].mean())
            for r, (x, y, rw, rh) in reg.items():
                e[r].append(diff[y:y + rh, x:x + rw].mean())
        err[(v, name)] = {c: (float(np.mean(e[c])), float(np.std(e[c]))) for c in cols}
        return err[(v, name)]

    # 1. the flow's motion, each way, for each picture size; and the replays that need nothing combined.
    #    Half size gets every way; DLAA and the fold get the ones that say how they compare (the
    #    full-size motion planes are 11 MB a frame each).
    def wanted(v, name):
        return v == "half-true" or name in ("supplied", "exact", "total", "correct-0", "correct-50", "correct-100") or \
            (v == "fold-cycle" and name == "alone-0")
    for v in FLOW_VARIANTS:
        for name, mode, loosen in FLOW_DUMPS:
            if wanted(v, name):
                do_dump(v, name, mode, loosen)
        for name in ["supplied", "exact"] + [n for n, _, _ in FLOW_DUMPS]:
            if wanted(v, name):
                do_replay(v, name)
                measure(v, name)

    # 2. which strictness is best for flow alone: lowest error on the swaying leaves at half size
    #    (where the motion matters most), the whole picture deciding a tie.
    def score(name):
        m = measure("half-true", name)
        if not m:
            return (1e9, 1e9)
        return (round(m["E-leaves-swaying"][0] + (m["F-leaves-gusting"][0] if "F-leaves-gusting" in m else 0.0), 2), m["whole"][0])
    alone = ["alone-0", "alone-50", "alone-100"]
    best = min(alone, key=score)
    note("Best strictness for flow alone (lowest error on the swaying and gusting leaves at half size): %s" % FLOW_LABELS[best])
    note("Combinations use that flow field. 'Supplied first, flow corrects' is run at all three strictness levels.")

    # 3. the two combinations made here from the best flow-alone field and the supplied motion.
    for v in ["half-true"]:
        w, h = size_of(v)
        src = dump_dir(v, best)
        if not os.path.isfile(os.path.join(src, "%05d-motion.raw" % (frames - 1))):
            continue
        for name in ("average", "flowfirst"):
            os.makedirs(dump_dir(v, name), exist_ok=True)
        for t in range(frames):
            if os.path.isfile(os.path.join(dump_dir(v, "flowfirst"), "%05d-motion.raw" % t)) and \
               os.path.isfile(os.path.join(dump_dir(v, "average"), "%05d-motion.raw" % t)):
                continue
            flow, prov = flow_planes(src, t, w, h)
            given = np.fromfile(os.path.join(scene_dir, v, "%05d-motion.raw" % t), dtype=np.float32).reshape(h, w, 2)
            known = (prov == 3)[..., None]  # the flow measured this pixel and its rules accepted it
            np.where(known, 0.5 * (flow + given), given).astype(np.float32).tofile(os.path.join(dump_dir(v, "average"), "%05d-motion.raw" % t))
            np.where(known, flow, given).astype(np.float32).tofile(os.path.join(dump_dir(v, "flowfirst"), "%05d-motion.raw" % t))
        for name in ("average", "flowfirst"):
            do_replay(v, name)
            measure(v, name)

    # 3b. Cort, 2026-11-25: "Perhaps we just need it to be blocked from estimating motion on still objects."
    #     Half size and the fold. The flow's motion is used only inside a zone and the supplied motion
    #     everywhere else. "Perfect" zone: where the exact motion differs from the supplied one (what a
    #     faultless detector would give; the upper limit of the idea). "Real" zone: where two frames,
    #     lined up with the supplied motion, still disagree (our own pictures only). The same zones are
    #     also given to DLSS as its "lean on this frame" mask with the supplied motion.
    zone_share = {}
    for v in ["half-true", "fold-cycle"]:
        w, h = size_of(v)
        k = W_ // w
        src = os.path.join(scene_dir, v)
        need = [dump_dir(v, "correct-100"), dump_dir(v, "alone-0")]
        if not all(os.path.isfile(os.path.join(d, "%05d-motion.raw" % (frames - 1))) for d in need):
            note("Block test skipped for %s: its flow planes are missing" % v)
            continue
        for sub in ("oracle", "detect"):
            os.makedirs(os.path.join(work, "bias", v, sub), exist_ok=True)
        for name in ("block-oracle", "block-oracle-alone", "block-detect"):
            os.makedirs(dump_dir(v, name), exist_ok=True)
        recent = []
        share = {c: {"oracle": [], "detect": []} for c in reg}
        for t in range(frames):
            given = np.fromfile(os.path.join(src, "%05d-motion.raw" % t), dtype=np.float32).reshape(h, w, 2)
            ex = np.fromfile(os.path.join(scene_dir, "exact", v, "%05d-motion.raw" % t), dtype=np.float32).reshape(h, w, 2)
            oracle = _box((np.hypot(ex[..., 0] - given[..., 0], ex[..., 1] - given[..., 1]) > 0.02).astype(np.float64), 2) > 0.0
            if t >= 1:
                recent = (recent + [_disagreement(src, t, w, h)])[-4:]
                detect = _box((np.mean(recent, 0) > DETECT_THRESHOLD).astype(np.float64), 2) > 0.0
            else:
                detect = np.zeros((h, w), dtype=bool)
            oracle.astype(np.float32).tofile(os.path.join(work, "bias", v, "oracle", "%05d-bias.raw" % t))
            detect.astype(np.float32).tofile(os.path.join(work, "bias", v, "detect", "%05d-bias.raw" % t))
            loose = flow_planes(dump_dir(v, "correct-100"), t, w, h)[0]
            alone0, prov0 = flow_planes(dump_dir(v, "alone-0"), t, w, h)
            np.where(oracle[..., None], loose, given).astype(np.float32).tofile(os.path.join(dump_dir(v, "block-oracle"), "%05d-motion.raw" % t))
            np.where((oracle & (prov0 == 3))[..., None], alone0, given).astype(np.float32).tofile(os.path.join(dump_dir(v, "block-oracle-alone"), "%05d-motion.raw" % t))
            np.where(detect[..., None], loose, given).astype(np.float32).tofile(os.path.join(dump_dir(v, "block-detect"), "%05d-motion.raw" % t))
            if t >= frames - last:
                for c, (x, y, rw, rh) in reg.items():
                    share[c]["oracle"].append(oracle[y // k:(y + rh) // k, x // k:(x + rw) // k].mean())
                    share[c]["detect"].append(detect[y // k:(y + rh) // k, x // k:(x + rw) // k].mean())
        zone_share[v] = {c: (float(np.mean(share[c]["oracle"])), float(np.mean(share[c]["detect"]))) for c in reg}
        for name in BLOCK_NAMES:
            do_replay(v, name)
            measure(v, name)

    # 4. numbers.
    pics = os.path.join(work, "compare")
    os.makedirs(pics, exist_ok=True)
    lines = ["Fold trial 4: our own optical flow on the made-up scene. The supplied motion has no leaf sway in it.",
             "Picture error against the exact picture (0..255 display scale), mean of the last %d frames. Lower is better." % last,
             "In brackets: how much that error varies from frame to frame.", ""]
    lines += notes + [""]
    for c in shown:
        lines.append("Region: %s" % c)
        lines.append("%-48s" % "motion given to DLSS" + "".join("%18s" % FLOW_VARIANT_NAMES[v] for v in FLOW_VARIANTS))
        for name in FLOW_ORDER:
            row = "%-48s" % (FLOW_LABELS[name] + (" *" if name == best else ""))
            for v in FLOW_VARIANTS:
                m = measure(v, name)
                row += "%18s" % ("%.2f (%.2f)" % m[c] if m else "-")
            lines.append(row)
        lines.append("")
    lines.append("* = the flow-alone strictness the two made-here combinations use.")
    lines.append("")
    for v in zone_share:
        lines.append("Share of each region inside the zone, %s: perfect zone / real zone (threshold %.3f)" % (FLOW_VARIANT_NAMES[v], DETECT_THRESHOLD))
        for c in [c for c in shown if c in zone_share[v]]:
            lines.append("    %-20s %3.0f%% / %3.0f%%" % (c, 100 * zone_share[v][c][0], 100 * zone_share[v][c][1]))
        lines.append("")

    # how good the motion itself is: distance from the exact motion, in full-size pixels, and how much
    # of the region the flow answered for.
    for c in [c for c in shown if c in ("F-leaves-gusting", "E-leaves-swaying", "D-leaves-still", "whole")]:
        lines.append("Motion error against the exact motion, full-size pixels, region %s (last %d frames); in brackets the share of pixels the flow answered for" % (c, last))
        lines.append("%-48s" % "motion" + "".join("%22s" % FLOW_VARIANT_NAMES[v] for v in FLOW_VARIANTS))
        for name in FLOW_ORDER:
            if name == "exact" or name.startswith("lean-"):
                continue
            row = "%-48s" % FLOW_LABELS[name]
            for v in FLOW_VARIANTS:
                w, h = size_of(v)
                k = W_ // w
                md = motion_dir(v, name) or os.path.join(scene_dir, v)
                if not os.path.isfile(os.path.join(md, "%05d-motion.raw" % (frames - 1))):
                    row += "%22s" % "-"
                    continue
                if c == "whole":
                    x0, y0, x1, y1 = 0, 0, w, h
                else:
                    x, y, rw, rh = reg[c]
                    x0, y0, x1, y1 = x // k, y // k, (x + rw) // k, (y + rh) // k
                dist, share = [], []
                for t in range(frames - last, frames):
                    mv, prov = flow_planes(md, t, w, h)
                    ex = np.fromfile(os.path.join(scene_dir, "exact", v, "%05d-motion.raw" % t), dtype=np.float32).reshape(h, w, 2)
                    dd = np.hypot(mv[..., 0] - ex[..., 0], mv[..., 1] - ex[..., 1])[y0:y1, x0:x1] * k
                    dist.append(dd.mean())
                    if prov is not None:
                        share.append(((prov[y0:y1, x0:x1] == 3) | (prov[y0:y1, x0:x1] == 4)).mean())
                row += "%22s" % ("%.3f px (%s)" % (np.mean(dist), "%.0f%%" % (100 * np.mean(share)) if share else "-"))
            lines.append(row)
        lines.append("")
    open(os.path.join(pics, "numbers.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))

    # 5. pictures: one sheet a region and picture size, every motion source side by side, last frame.
    for c in [c for c in shown if c in ("F-leaves-gusting", "E-leaves-swaying", "D-leaves-still", "A-wire")]:
        x, y, rw, rh = reg[c]
        for v in FLOW_VARIANTS:
            panels = [("Exact picture", truth[frames - 1])]
            if ("native", "supplied") in final and v != "native":
                panels.append(("DLAA, supplied motion", final[("native", "supplied")]))
            for name in FLOW_ORDER:
                if (v, name) in final:
                    panels.append(("%s: %s  [%.2f]" % (FLOW_VARIANT_NAMES[v], FLOW_LABELS[name], err[(v, name)][c][0]), final[(v, name)]))
            per_row = 4
            rows_n = (len(panels) + per_row - 1) // per_row
            sheet = Image.new("RGB", ((rw * 2 + 8) * per_row, (rh * 2 + 30) * rows_n), (24, 24, 24))
            dr = ImageDraw.Draw(sheet)
            for k, (label, pic) in enumerate(panels):
                crop = Image.fromarray(np.clip(pic[y:y + rh, x:x + rw], 0, 255).astype(np.uint8)).resize((rw * 2, rh * 2), Image.NEAREST)
                ox, oy = (k % per_row) * (rw * 2 + 8), (k // per_row) * (rh * 2 + 30)
                dr.text((ox + 4, oy + 8), label, fill=(255, 255, 255))
                sheet.paste(crop, (ox, oy + 30))
            sheet.save(os.path.join(pics, "compare-%s-%s.jpg" % (c, v)), quality=92)

    # A script Cort can run to remove the bulky in-between motion planes (the results stay).
    with open(os.path.join(work, "DELETE-flow-planes.cmd"), "w") as f:
        f.write("@echo off\r\nrem Removes only the in-between motion planes of fold trial 4. Numbers and pictures stay.\r\n"
                "rmdir /s /q \"%~dp0flow\"\r\necho Done.\r\npause\r\n")
    return 0



def clip(scene_dir, out_gif, region, panels, scale=2, fps=8):
    """A moving picture for Cort to judge in motion: one region of every given replay, side by side,
    every saved frame in order. panels: "Label=folder" (a replay output folder of NNNNN-dlssout.raw)."""
    from PIL import Image, ImageDraw
    info = json.load(open(os.path.join(scene_dir, "scene.json")))
    w, h = info["width"], info["height"]
    x, y, rw, rh = regions(w, h, info.get("gust", 0) > 0)[region]
    cols = []
    for p in panels:
        label, folder = p.split("=", 1)
        files = sorted(f for f in os.listdir(folder) if f.endswith("-dlssout.raw"))
        cols.append((label, folder, files))
    n = min(len(c[2]) for c in cols)
    pw, ph = rw * scale, rh * scale
    frames = []
    for i in range(n):
        sheet = Image.new("RGB", ((pw + 6) * len(cols), ph + 26), (24, 24, 24))
        dr = ImageDraw.Draw(sheet)
        for k, (label, folder, files) in enumerate(cols):
            raw = np.fromfile(os.path.join(folder, files[len(files) - n + i]), dtype=np.float16).reshape(h, w, 4)[..., :3]
            crop = np.clip(display(raw)[y:y + rh, x:x + rw], 0, 255).astype(np.uint8)
            sheet.paste(Image.fromarray(crop).resize((pw, ph), Image.NEAREST), (k * (pw + 6), 26))
            dr.text((k * (pw + 6) + 4, 6), label, fill=(255, 255, 255))
        frames.append(sheet)
    frames[0].save(out_gif, save_all=True, append_images=frames[1:], duration=int(1000 / fps), loop=0)
    print("clip:", out_gif, n, "frames")


if __name__ == "__main__":
    args = sys.argv[1:]
    if len(args) >= 2 and any(a.startswith("-") and not a[1:2].isdigit() for a in args[1:]):
        print("This script takes folders and numbers only, no options:", " ".join(args[1:]))
        sys.exit(2)
    if len(args) >= 2 and args[0] == "generate":
        safe_cores()
        generate(args[1], int(args[2]) if len(args) > 2 else FRAMES, (int(args[3]), int(args[4])) if len(args) > 4 else None)
    elif len(args) >= 2 and args[0] == "generate-leafmotion":
        # The same scene, with the swaying leaves' motion in the vectors: DLAA, ordinary half size, the fold.
        safe_cores()
        LEAF_MOTION = True
        generate(args[1], int(args[2]) if len(args) > 2 else FRAMES, None, ["native", "half-true", "fold-cycle"])
    elif len(args) >= 2 and args[0] == "generate-flowtrial":
        # Trial 4's scene: DLAA, ordinary half size and the fold, supplied motion without the sway, and the
        # exact motion beside each.
        safe_cores()
        GUST = 4.0
        USE_R2 = True
        generate(args[1], int(args[2]) if len(args) > 2 else FRAMES, (int(args[3]), int(args[4])) if len(args) > 4 else None,
                 FLOW_VARIANTS, exact=True)
    elif len(args) >= 2 and args[0] == "generate-mip":
        safe_cores()
        generate_mip(args[1], int(args[2]) if len(args) > 2 else FRAMES, (int(args[3]), int(args[4])) if len(args) > 4 else None)
    elif len(args) >= 2 and args[0] == "generate-final":
        # The combined test: R2 jitter, the gusting band, the exact motion beside each variant, and the
        # given mip bias for the half-size and folded pictures (DLAA keeps -1).
        safe_cores()
        GUST = 4.0
        USE_R2 = True
        MIP_BIAS = float(args[2]) if len(args) > 2 else -2.0
        generate(args[1], int(args[3]) if len(args) > 3 else FRAMES, None, FLOW_VARIANTS, exact=True)
    elif len(args) >= 5 and args[0] == "clip":
        safe_cores()
        clip(args[1], args[2], args[3], args[4:])
    elif len(args) >= 4 and args[0] == "flowtrial":
        safe_cores()
        sys.exit(flowtrial(args[1], args[2], args[3]))
    elif len(args) >= 3 and args[0] == "evaluate":
        evaluate(args[1], args[2])
    else:
        print(__doc__)
        sys.exit(2)
