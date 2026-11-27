"""Offline lens readout from captured depth and the motion delivered to DLAA.

Uses the eleven polynomial terms and 2/height coordinates in screen_motion.hpp.
This is a robust refit of the delivered field, not a dump of the GPU's coefficients.
Camera matrices are opened by the scoring path only. The fit assumes a centered
perspective projection, square pixels and motion in unjittered scene coordinates.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

import numpy as np


def fit_field(x, y, q, flow):
    """Return robust per-frame coefficients in pixels, or a refusal reason."""
    keep = np.isfinite(flow).all(axis=1) & np.isfinite(q) & (q > 0) & (q < 1)
    keep &= np.max(np.abs(flow), axis=1) < 512
    x, y, q, flow = x[keep], y[keep], q[keep], flow[keep]
    if len(q) < 256 or np.ptp(y) < 1 or np.ptp(x) < 1:
        return None, "insufficient scene coverage"
    if np.percentile(np.linalg.norm(flow, axis=1), 60) < 0.35:
        return None, "insufficient screen motion"
    n = len(q)
    design = np.zeros((n, 2, 11))
    design[:, 0, [0, 1, 2, 6, 7, 8, 10]] = np.column_stack((np.ones(n), x, y, x*x, x*y, q, q*x))
    design[:, 1, [3, 4, 5, 6, 7, 9, 10]] = np.column_stack((np.ones(n), x, y, x*y, y*y, q, q*y))
    mat = design.reshape(-1, 11)
    scale = np.linalg.norm(mat, axis=0)
    if np.min(scale) < 1e-9:
        return None, "depth or spatial terms unobservable"
    mat = mat / scale
    weight = np.ones(n)
    for band in (6, 2, 0.75, 0.4):
        wt = np.repeat(np.sqrt(weight), 2)
        coef, _, rank, sv = np.linalg.lstsq(mat * wt[:, None], flow.ravel() * wt, rcond=1e-7)
        if rank != 11 or sv[-1] / sv[0] < 0.0001:
            return None, "ill-conditioned screen fit"
        theta = coef / scale
        err = np.linalg.norm((design @ theta) - flow, axis=1)
        weight = 1 / (1 + (err / band)**2)**2
    inlier = err < 0.75
    if inlier.mean() < .70 or any(np.sum(inlier & (x*s > 0) & (y*t > 0)) < 20
                                  for s in (-1, 1) for t in (-1, 1)):
        return None, "background fit lacks spatial agreement"
    return theta, "usable"


def no_lens(why, **diag):
    return dict(state="no lens yet", why=why, **diag)


def axis_estimate(turns,offset):
    axis_f = []
    for rotation, shift, translation in [(6, 0, 8), (7, 3, 9)]:
        use = turns[np.abs(turns[:, rotation]) > .35]
        if len(use) < 6:
            return no_lens("insufficient rotation on one axis", turn_frames=len(turns))
        rot = use[:, rotation]
        estimates = (use[:, shift] - offset * use[:, translation]) / rot
        if np.min(estimates) <= 0:
            return no_lens("inconsistent rotation sign")
        focal = np.sqrt(estimates)
        if (np.percentile(focal, 90) - np.percentile(focal, 10)) / np.median(focal) > .05:
            return no_lens("lens changed or motion fit unstable within turn")
        axis_f.append(float(np.median(focal)))
    disagreement = abs(axis_f[0]-axis_f[1])/np.mean(axis_f)
    f = float(np.mean(axis_f))
    vfov = float(np.degrees(2*np.arctan(1/f)))
    if disagreement > .02 or not 30 <= vfov <= 110:
        return no_lens("pan and tilt disagree or field of view outside test range")
    return dict(state="focal only",focal_normalized=f,vfov_degrees=vfov,axis_disagreement=float(disagreement))


def readout(fields, support=()):
    """Turns supply focal evidence. A separate same-view slide can constrain A."""
    if not len(fields):
        return no_lens("no usable motion fields")
    fields = np.asarray(fields)
    turns = fields[np.max(np.abs(fields[:, 6:8]), axis=1) > .35]
    if len(turns) < 6:
        return no_lens("no sustained turn", turn_frames=len(turns))
    # At field level a6,a7 are rotation, and c1,c2,c3 are translation in q.
    # Win a full lens only if the depth term has a measurable effect.
    all_fields = np.vstack((turns, support)) if len(support) else turns
    rows, values = [], []
    for a in all_fields:
        rows.extend(((a[6], a[8]), (a[7], a[9]), (0, a[10]), (0, a[10])))
        values.extend((a[0], a[3], a[1], a[5]))
    design = np.asarray(rows)
    column = np.linalg.norm(design, axis=0)
    if column[1] / np.sqrt(len(design)) < 3.0:
        # Pure rotation still constrains focal length. Score it conditionally
        # while explicitly withholding the full lens and the depth offset.
        turn_only = np.array(turns,copy=True)
        turn_only[:,8:] = 0
        diagnostic = axis_estimate(turn_only, 0)
        return no_lens("turn observed, depth offset lacks parallax", turn_frames=len(turns),
                       rotation_only=diagnostic)
    coeff, _, rank, sv = np.linalg.lstsq(design / column, values, rcond=1e-7)
    if rank != 2 or sv[-1] < 0.1 * sv[0]:
        return no_lens("rotation and translation cannot be separated")
    f2, offset = coeff / column
    if f2 <= 0 or not -1e-5 <= offset <= .25:
        return no_lens("nonphysical or unsupported perspective solution")
    offset = max(0., float(offset))
    axes = axis_estimate(turns,offset)
    if axes["state"] != "focal only":
        return axes
    disagreement = axes["axis_disagreement"]
    f = float(np.sqrt(f2))
    vfov = float(np.degrees(2*np.arctan(1/f)))
    if abs(f/axes["focal_normalized"]-1) > .02:
        return no_lens("joint fit and rotation axes disagree")
    resid = np.asarray(values) - design @ [f2, offset]
    if np.sqrt(np.mean(resid**2)) > .22:
        return no_lens("field equations leave excessive residual")
    return dict(state="candidate", focal_normalized=f, vfov_degrees=vfov, depth_offset=offset,
                turn_frames=len(turns), axis_disagreement=float(disagreement))


def plane(prefix, desc, dtype, channels):
    width, height, pitch = (int(desc[k]) for k in ("width", "height", "row_pitch"))
    data = prefix.with_name(prefix.name + desc["name"] + ".raw")
    unit = np.dtype(dtype).itemsize
    if min(width, height) <= 0 or pitch < width*channels*unit or pitch % unit or data.stat().st_size < pitch*(height-1)+width*channels*unit:
        raise ValueError("plane footprint mismatch")
    mapped = np.memmap(data, dtype, "r")
    return np.ndarray((height,width,channels), dtype=dtype, buffer=mapped,
                      strides=(pitch,channels*unit,unit))


def observations(meta, path):
    if meta.get("camera_cut") or meta.get("reset") or not meta.get("evaluation_succeeded", True):
        raise ValueError("reset, cut or failed evaluation")
    if meta.get("dlss", {}).get("mv_jittered") is not False:
        raise ValueError("unverified jitter convention")
    rev = meta["depth_reversed"]
    if rev not in (0, 1) or meta.get("dlss", {}).get("depth_inverted") != bool(rev):
        raise ValueError("depth convention disagreement")
    planes = {p["name"]: p for p in meta["planes"]}
    d, m = planes["depth"], planes["motion"]
    if d["dxgi_format"] not in (40, 41) or m["dxgi_format"] not in (16, 34):
        raise ValueError("unsupported depth or motion format")
    w, h = meta["size"]
    if any((p["width"],p["height"]) != (w,h) for p in (d,m)):
        raise ValueError("unregistered planes")
    scale = np.asarray(meta["mv_scale"])
    if scale.shape != (2,) or not np.isfinite(scale).all() or (scale == 0).any():
        raise ValueError("invalid motion units")
    prefix = path.with_name(path.name.removesuffix("meta.json"))
    depth = plane(prefix,d,"<f4",1)
    motion = plane(prefix,m,{16:"<f4",34:"<f2"}[m["dxgi_format"]],2)
    yy,xx = np.meshgrid(np.linspace(.04*h,.96*h,32).astype(int),
                         np.linspace(.04*w,.96*w,56).astype(int), indexing="ij")
    xx, yy = xx.ravel(), yy.ravel()
    q = depth[yy,xx,0].astype(float)
    return (2*(xx+.5-w/2)/h, 2*(yy+.5-h/2)/h, q if rev else 1-q, motion[yy,xx].astype(float)*scale)


def reference(meta):
    """Fresh, invertible centered perspective only. Held views and guesses abstain."""
    c = meta.get("camera", {})
    if not c.get("valid") or c.get("age") != 0 or c.get("source",0) <= 0:
        return None
    try:
        vp = np.asarray(c["vp_current_row"]).reshape(4,4)
        inv = np.asarray(c["inv_vp_current_row"]).reshape(4,4)
        if not np.allclose(vp @ inv,np.eye(4),atol=1e-3):
            return None
        w, h = meta["size"]
        forward = vp[:3,3]
        unit = np.linalg.norm(forward)
        if unit < 1e-6:
            return None
        direction = forward / unit
        x,y,z = vp[:3,:3].T / unit
        if max(abs(x@direction), abs(y@direction)) > .005 or abs(x@y) > .005*np.linalg.norm(x)*np.linalg.norm(y):
            return None
        f = np.linalg.norm(y)
        if abs(np.linalg.norm(x)*w/h/f-1) > .01:
            return None
        inf = z@direction
        a = -inf if meta["depth_reversed"] else inf-1
        if not np.isfinite([f,a]).all() or a < -1e-5 or a > .25:
            return None
        return float(f), max(0.,float(a))
    except (KeyError, ValueError, TypeError, FloatingPointError):
        return None


def inspect(directory):
    fields, refs, rejected, scope = [], [], Counter(), set()
    indexed = sorted(directory.glob("*-meta.json"))
    for p in indexed:
        try:
            meta = json.loads(p.read_text(encoding="utf-8-sig"))
            scope.add((meta["exe"].lower(), meta["pid"], tuple(meta["size"]), meta["depth_reversed"]))
            ref = reference(meta)
            if ref is not None:
                refs.append((p.name, *ref))
            a, why = fit_field(*observations(meta,p))
            if a is None:
                rejected[why] += 1
            else:
                fields.append(a)
        except (KeyError, OSError, ValueError, TypeError) as err:
            rejected[str(err)] += 1
    if len(scope) > 1:
        rejected["changed view dimensions, depth convention or process"] += len(fields)
        fields, refs = [], []
    diag = dict(folder=directory.name, frames=len(indexed), usable=len(fields), reference_frames=len(refs),
                rejected=dict(rejected), independent=readout(fields))
    if refs:
        guess = 1 / np.tan(np.radians(25))
        errors = [abs(guess/f-1) for _,f,_ in refs]
        diag["fifty_degree_control"] = dict(reference_frames=len(errors),within_1pct=int(np.sum(np.asarray(errors)<=.01)),
                                            median_relative_focal_error=float(np.median(errors)))
    return fields, refs, scope, diag


def compare(directory, slide=None, repeat=None):
    fields, refs, scope, result = inspect(directory)
    result["estimate"] = result["independent"]
    support = []
    for path in (slide, repeat):
        if path is not None and path.resolve() == directory.resolve():
            raise ValueError("Each condition requires a different recording")
    if slide and repeat and slide.resolve() == repeat.resolve():
        raise ValueError("Slide and repeat share a recording")
    if slide:
        extra, _, extra_scope, ctrl = inspect(slide)
        result["slide"] = ctrl
        if len(scope) != 1 or scope != extra_scope or len(extra) < 6:
            result["estimate"] = no_lens("missing same-session slide evidence")
            return result
        if np.percentile(np.max(np.abs(np.asarray(extra)[:,6:8]), axis=1),90) > .35:
            result["estimate"] = no_lens("slide also contains a turn")
            return result
        support = extra
        result["estimate"] = readout(fields,support)
    candidate = result["estimate"]
    focal_result = candidate if candidate["state"] == "candidate" else candidate.get("rotation_only",{})
    if "focal_normalized" in focal_result:
        result["focal_reference"] = [dict(frame=name,relative_error=abs(focal_result["focal_normalized"]/rf-1)) for name,rf,_ in refs]
    if candidate["state"] == "candidate":
        f, a = candidate["focal_normalized"], candidate["depth_offset"]
        result["reference_comparison"] = [
            dict(frame=name, focal_relative_error=abs(f/rf-1), offset_absolute_error=abs(a-ra),
                 offset_relative_error=(abs(a/ra-1) if ra>1e-7 else None),
                 focal_within_1pct=abs(f/rf-1)<=.01,
                 offset_within_10pct=(abs(a/ra-1)<=.1 if ra>1e-7 else None))
            for name,rf,ra in refs]
    if repeat:
        second, _, second_scope, second_diag = inspect(repeat)
        result["repeat"] = second_diag
        other = readout(second,support) if len(scope)==1 and scope == second_scope else no_lens("different camera session")
        result["repeat_estimate"] = other
        second_focal = other if other["state"] == "candidate" else other.get("rotation_only",{})
        if "focal_normalized" in focal_result and "focal_normalized" in second_focal:
            error = abs(focal_result["focal_normalized"]/second_focal["focal_normalized"]-1)
            result["turn_repeat"] = dict(focal_relative_difference=error, within_1pct=error<=.01,
                                          shared_slide=bool(support))
    return result


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("captures", type=Path, nargs="+")
    p.add_argument("--slide",type=Path)
    p.add_argument("--repeat",type=Path)
    p.add_argument("--output",type=Path)
    args = p.parse_args()
    if any(not path.is_dir() for path in [*args.captures, *filter(None,[args.slide,args.repeat])]):
        p.error("capture directory missing")
    if len(args.captures)!=1 and (args.slide or args.repeat):
        p.error("supply one turn when comparing with slide or repeat")
    report = [compare(d,args.slide,args.repeat) for d in args.captures]
    output = json.dumps(report,indent=2,allow_nan=False)
    if args.output:
        args.output.write_text(output+"\n",encoding="utf-8")
    else:
        print(output)

