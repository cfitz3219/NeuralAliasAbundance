"""Offline perspective calibration from matched depths of rigid scene points.

Each observation compares two points seen in two frames. The last axis contains
(disparity, x, y). Use normalized viewport coordinates, with x multiplied by
width/height, y up, and disparity = d for reversed depth or 1-d for normal depth.
Input NPZ fields: observations [N,4,3] ordered A,B,previous A,previous B;
frame_pair [N] identifies the two camera poses. Units of view depth remain arbitrary.
Requires NumPy. The output describes a fit. Lighting accuracy requires an independent
effect reference. This diagnostic never enables a runtime feature.
"""

import argparse
import json
from pathlib import Path

import numpy as np


def fit_at_offset(rows, offset):
    z = 1.0 / (rows[:, :, 0] + offset)
    xyz = np.stack((rows[:, :, 1] * z, rows[:, :, 2] * z, z), axis=-1)
    current = xyz[:, 0] - xyz[:, 1]
    previous = xyz[:, 2] - xyz[:, 3]

    def squared_distance_terms(deltas):
        x, y, z = deltas.T
        return np.column_stack((x*x + y*y, 2*x*z, 2*y*z, z*z))

    # A rigid camera transform preserves pairwise distances. Subtracting the
    # two distance equations eliminates each pair's unknown physical length.
    equations = squared_distance_terms(current) - squared_distance_terms(previous)
    normalizer = np.maximum((current**2 + previous**2).sum(axis=1), 1e-9)
    equations /= normalizer[:, None]
    design = equations[:, 1:]
    target = -equations[:, 0]
    weights = np.ones(len(rows))
    for _ in range(4):
        coeff, _, rank, singular = np.linalg.lstsq(
            design * weights[:, None], target * weights, rcond=1e-9)
        if rank < 3 or singular[0] / singular[-1] > 1e6:
            return None
        residual = np.abs(design @ coeff - target)
        weights = np.minimum(1.0, (2*np.median(residual) + 1e-9) /
                             np.maximum(residual, 1e-10))
    x, y, zz = coeff
    focal_squared = zz - x*x - y*y
    if focal_squared <= 0:
        return None
    return dict(focal_y=float(np.sqrt(focal_squared)), center_x_aspect=float(-x),
                center_y=float(-y), far_offset=float(offset),
                median_residual=float(np.median(residual)))


def fit_projection(rows):
    rows = np.asarray(rows, dtype=np.float64)
    if rows.ndim != 3 or rows.shape[1:] != (4, 3):
        raise ValueError("Expected observations of shape [N,4,3]")
    valid = np.isfinite(rows).all(axis=(1, 2)) & (rows[:, :, 0] > 0).all(axis=1)
    rows = rows[valid]
    if len(rows) < 4:
        return None
    if np.max(np.abs(rows[:, :2] - rows[:, 2:])) < 1e-12:
        return None

    # These are search bounds for this experiment. An optimum on the boundary
    # needs a wider independent search before interpreting the parameters.
    grid = np.r_[0.0, np.geomspace(1e-7, 0.1, 23)]
    models = [fit_at_offset(rows, offset) for offset in grid]

    def loss(model):
        return model["median_residual"] if model is not None else np.inf

    best = min(range(len(grid)), key=lambda i: loss(models[i]))
    if models[best] is None:
        return None
    low, high = grid[max(0, best-1)], grid[min(len(grid)-1, best+1)]
    for _ in range(38):
        left = low + (high-low)/3
        right = high - (high-low)/3
        if loss(fit_at_offset(rows, left)) < loss(fit_at_offset(rows, right)):
            high = right
        else:
            low = left
    return min((models[best], fit_at_offset(rows, (low+high)/2)), key=loss)


def compare_splits(rows, frame_pair):
    ids = np.asarray(frame_pair)
    if ids.ndim != 1 or len(ids) != len(rows):
        raise ValueError("Expected one frame_pair ID for each observation")
    groups = np.unique(ids)
    if len(groups) < 3 or len(rows) < 180:
        return dict(status="no reliable input", reason="insufficient tracks or frame pairs")
    fits = [fit_projection(rows[np.isin(ids, group)]) for group in (groups[::2], groups[1::2])]
    if any(fit is None for fit in fits):
        return dict(status="no reliable input", reason="physical solution missing in a split", fits=fits)
    return dict(status="diagnostic only", fits=fits,
                relative_focal_difference=abs(fits[0]["focal_y"]/fits[1]["focal_y"]-1))


def score_layer(predicted, reference, scene, threshold=0.001):
    predicted, reference, scene = map(np.asarray, (predicted, reference, scene))
    if predicted.shape != reference.shape or scene.shape != reference.shape:
        raise ValueError("Supply registered prediction, reference and scene mask with the same shape")
    scene = scene.astype(bool)
    if not (np.isfinite(predicted[scene]).all() and np.isfinite(reference[scene]).all()):
        raise ValueError("Nonfinite scene samples")
    active = scene & (np.abs(reference) > threshold)
    called = scene & (np.abs(predicted) > threshold)
    same = active & called & (np.sign(predicted) == np.sign(reference))
    within = same & (np.abs(predicted-reference) <= .01*np.abs(reference))
    affected, proposed = int(active.sum()), int(called.sum())
    if affected == 0:
        return dict(status="no reference response", affected=0, proposed=proposed)
    scores = dict(precision=float(same.sum()/max(1, proposed)), recall=float(same.sum()/affected),
                  strength=float(within.sum()/affected))
    within_floor = same & (np.abs(predicted-reference) <= np.maximum(.01*np.abs(reference), .005))
    return dict(status="pass" if min(scores.values()) >= .99 else "fail",
                strength_at_005_stop_floor=float(within_floor.sum()/affected),
                affected=affected, proposed=proposed, **scores)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="NPZ observations or registered effect layers")
    parser.add_argument("--score", action="store_true",
                        help="Score predicted, reference and scene fields instead of fitting geometry")
    args = parser.parse_args()
    with np.load(args.input, allow_pickle=False) as data:
        if args.score:
            result = score_layer(data["predicted"], data["reference"], data["scene"])
        else:
            result = compare_splits(data["observations"], data["frame_pair"])
    print(json.dumps(result, indent=2, allow_nan=False))
