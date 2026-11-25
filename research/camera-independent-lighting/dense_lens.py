"""Blind lens from a camera turn: dense picture matches, both depth buffers, exact rigid motion.

Offline research tool (2026-11-23). It reads colour, depth and the raster jitter of saved J2 captures.
It never opens a camera matrix or the delivered motion vectors. Unknowns: one focal length F (in
picture half-heights, so vertical FOV = 2 atan(1/F)) and one depth offset A shared by all pairs
(1/distance is proportional to q + A, q = d for reversed depth, 1 - d otherwise), plus one rigid
pose (rotation, translation) per frame pair. Assumes a centred perspective lens with square pixels.

Differences from image_depth_lens.py: about 10,000 to 25,000 matches per pair from dense optical flow
(instead of tens of corners), a continuous least-squares fit of F and A (no grid), and the raster
jitter is removed from both pictures' coordinates (position = pixel centre - raster_jitter_px).

  python dense_lens.py CAPTURE_DIR 0-2 2-4 0-4 [--jitter -1] [--reference-vfov 50]
  python dense_lens.py --selftest
Requires numpy, scipy, opencv-python-headless.
"""
import argparse, json, os
import numpy as np


def plane(base, meta, name, dtype, ch):
    p = [p for p in meta["planes"] if p["name"] == name][0]
    w, h, pitch = p["width"], p["height"], p["row_pitch"]
    a = np.fromfile(base + name + ".raw", dtype=np.uint8)
    if a.size < h * pitch:  # the last row is stored without its padding
        a = np.concatenate([a, np.zeros(h * pitch - a.size, np.uint8)])
    return a[:h * pitch].reshape(h, pitch)[:, :w * ch * np.dtype(dtype).itemsize].copy().view(dtype).reshape(h, w, ch)


_cache = {}


def frame(folder, i):
    if (folder, i) in _cache:
        return _cache[(folder, i)]
    base = os.path.join(folder, f"{i:05d}-")
    meta = json.load(open(base + "meta.json"))
    col = plane(base, meta, "color", np.float16, 4)[..., :3].astype(np.float32)
    y = np.log1p(np.maximum(col @ np.array([0.2126, 0.7152, 0.0722], np.float32), 0) * 80)
    y = 0.25 * (y[0::2, 0::2] + y[1::2, 0::2] + y[0::2, 1::2] + y[1::2, 1::2])
    lo, hi = np.percentile(y, [0.5, 99.5])
    grey = (np.clip((y - lo) / (hi - lo), 0, 1) * 255).astype(np.uint8)
    dep = plane(base, meta, "depth", np.float32, 1)[..., 0].astype(np.float64)
    q = dep if meta.get("depth_reversed") else 1 - dep
    _cache[(folder, i)] = (meta, grey, q)
    return _cache[(folder, i)]


def matches(folder, i, j, step=6, jitter_sign=-1.0):
    """Rows of (x1, y1, q1, x2, y2, q2); x and y from the picture centre in half-heights."""
    import cv2
    mi, gi, qi = frame(folder, i)
    mj, gj, qj = frame(folder, j)
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    dis.setFinestScale(0)
    fw = dis.calc(gi, gj, None)
    bw = dis.calc(gj, gi, None)
    h, w = gi.shape
    H, W = qi.shape
    ys, xs = np.mgrid[step:h - step:step, step:w - step:step]
    xs = xs.ravel().astype(np.float64)
    ys = ys.ravel().astype(np.float64)
    f = fw[ys.astype(int), xs.astype(int)]
    x2, y2 = xs + f[:, 0], ys + f[:, 1]
    ok = (x2 > 2) & (x2 < w - 3) & (y2 > 2) & (y2 < h - 3)
    xi = np.clip(np.rint(x2).astype(int), 0, w - 1)
    yi = np.clip(np.rint(y2).astype(int), 0, h - 1)
    b = bw[yi, xi]
    ok &= np.hypot(f[:, 0] + b[:, 0], f[:, 1] + b[:, 1]) < 0.3      # forward and backward flow agree
    gx = cv2.Sobel(gi, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(gi, cv2.CV_32F, 0, 1, ksize=3)
    ok &= cv2.boxFilter(gx * gx + gy * gy, -1, (7, 7))[ys.astype(int), xs.astype(int)] > 60   # textured
    X1, Y1, X2, Y2 = 2 * xs + 1, 2 * ys + 1, 2 * x2 + 1, 2 * y2 + 1  # full-size pixel centres

    def depth_at(q, X, Y):
        cx = np.clip(np.floor(X).astype(int), 3, W - 4)
        cy = np.clip(np.floor(Y).astype(int), 3, H - 4)
        nb = np.stack([q[cy + a, cx + c] for a in (-3, 0, 3) for c in (-3, 0, 3)])
        return q[cy, cx], nb.max(0) - nb.min(0)
    q1, r1 = depth_at(qi, X1, Y1)
    q2, r2 = depth_at(qj, X2, Y2)
    ok &= (q1 > 1e-7) & (q2 > 1e-7) & (r1 < 0.03 * q1) & (r2 < 0.03 * q2)   # away from depth edges and sky
    j1 = jitter_sign * np.array(mi["raster_jitter_px"])
    j2 = jitter_sign * np.array(mj["raster_jitter_px"])
    S = H / 2.0
    return np.column_stack([(X1 + j1[0] - W / 2) / S, (Y1 + j1[1] - H / 2) / S, q1,
                            (X2 + j2[0] - W / 2) / S, (Y2 + j2[1] - H / 2) / S, q2])[ok], S


def residuals(p, data, S, depth_weight):
    from scipy.spatial.transform import Rotation
    F, A = np.exp(p[0]), p[1] * 1e-4
    out = []
    for M, pose in zip(data, p[2:].reshape(len(data), 6)):
        z = 1.0 / (M[:, 2] + A)
        P = np.column_stack([M[:, 0] / F * z, M[:, 1] / F * z, z])
        P2 = P @ Rotation.from_rotvec(pose[:3]).as_matrix().T + pose[3:] * 10.0
        zz = np.maximum(P2[:, 2], 1e-6)
        out += [(F * P2[:, 0] / zz - M[:, 3]) * S, (F * P2[:, 1] / zz - M[:, 4]) * S,
                depth_weight * 200 * np.log(np.maximum((1 / zz) / (M[:, 5] + A), 1e-9))]
    return np.concatenate(out)


def solve(pairs, S=720.0, depth_weight=0.5, nmax=4000, seed=0, starts=(1.3, 2.0, 3.2)):
    """Returns F, A, poses, median |residual| in pixels. Three starting lenses; the lowest cost wins."""
    from scipy.optimize import least_squares
    rng = np.random.default_rng(seed)
    data = [M[rng.choice(len(M), min(nmax, len(M)), replace=False)] for M in pairs]
    best = None
    for F0 in starts:
        p = np.concatenate([[np.log(F0), 0.0], np.zeros(6 * len(data))])
        r = least_squares(lambda x: residuals(np.concatenate([p[:2], x]), data, S, depth_weight), p[2:],
                          loss="soft_l1", f_scale=1.0, max_nfev=60)     # poses first, lens held
        p[2:] = r.x
        r = least_squares(residuals, p, args=(data, S, depth_weight), loss="soft_l1", f_scale=0.5,
                          x_scale="jac", max_nfev=150)
        if best is None or r.cost < best.cost:
            best = r
    F, A = np.exp(best.x[0]), best.x[1] * 1e-4
    return F, A, best.x[2:].reshape(len(data), 6), float(np.median(np.abs(best.fun)))


def selftest():
    from scipy.spatial.transform import Rotation
    rng = np.random.default_rng(3)
    F, A = 1 / np.tan(np.radians(30)), 0.001
    pairs = []
    for rot, t in (([0.01, -0.05, 0.002], [0.4, 0.1, 0.2]), ([-0.02, 0.06, 0.0], [-0.3, 0.0, 0.5])):
        xy = rng.uniform([-2.3, -1], [2.3, 1], (6000, 2))
        q = rng.uniform(0.002, 0.05, 6000)
        z = 1 / (q + A)
        P = np.column_stack([xy[:, 0] / F * z, xy[:, 1] / F * z, z]) @ Rotation.from_rotvec(rot).as_matrix().T + np.array(t) * 10
        xy2 = F * P[:, :2] / P[:, 2:3] + rng.normal(0, 0.15 / 720, (6000, 2))
        pairs.append(np.column_stack([xy, q, xy2, 1 / P[:, 2] - A]))
    f, a, _, _ = solve(pairs)
    assert abs(f / F - 1) < 2e-3 and abs(a - A) < 1e-4, (f, a)
    print(f"selftest: known 60.000 degree lens, offset 0.001000 -> {np.degrees(2 * np.arctan(1 / f)):.3f} degrees, {a:.6f}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", nargs="?")
    ap.add_argument("pairs", nargs="*", help="frame index pairs such as 0-2")
    ap.add_argument("--jitter", type=float, default=-1.0, help="sign applied to raster_jitter_px (0 ignores it)")
    ap.add_argument("--reference-vfov", type=float, help="for scoring only; never used by the fit")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        raise SystemExit
    ref = 1 / np.tan(np.radians(a.reference_vfov / 2)) if a.reference_vfov else None
    pairs = [tuple(map(int, s.split("-"))) for s in a.pairs]
    data = []
    for i, j in pairs:
        M, S = matches(a.folder, i, j, jitter_sign=a.jitter)
        data.append(M)
        print(f"pair {i}-{j}: {len(M)} matches, median shift {np.median(np.hypot(M[:, 3] - M[:, 0], M[:, 4] - M[:, 1]) * S):.1f} px")
    for name, dd in [("all pairs", data)] + [(f"pair {i}-{j}", [m]) for (i, j), m in zip(pairs, data)]:
        F, A, poses, med = solve(dd, S=S)
        err = f"  focal error {100 * (F / ref - 1):+.2f}%" if ref else ""
        turn = np.degrees(np.linalg.norm(poses[:, :3], axis=1)).round(2)
        print(f"{name:10s} vfov {np.degrees(2 * np.arctan(1 / F)):.3f}{err}  offset {A:.6f}  median residual {med:.3f} px  turn {turn} deg", flush=True)
