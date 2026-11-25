"""Explore a centered pinhole lens using texture matches and two raw depth images.

This offline experiment uses a grid of lens and depth-offset hypotheses. For each
hypothesis it fits rigid camera motion to two-thirds of the picture matches and
scores the remaining third. Assumes square pixels and fixed intrinsics. Its output
is a candidate for comparison with reference cameras, never a runtime approval.
The score uses normalized picture coordinates scaled to a 360-pixel-high image.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from lens_readout import plane

def read_frame(folder, i):
    prefix = folder / f'{i:05}-'
    metadata = json.loads((folder / f'{i:05}-meta.json').read_text())
    descriptors = {color_desc['name']: color_desc for color_desc in metadata['planes']}
    color_desc = descriptors['color']
    if not metadata.get('frame_closed') or not metadata.get('evaluation_succeeded') or metadata.get('reset') or metadata.get('camera_cut') or (color_desc['dxgi_format'] != 10) or (descriptors['depth']['dxgi_format'] not in (40, 41)) or (metadata.get('depth_reversed') not in (0, 1)) or (metadata['dlss'].get('depth_inverted') != bool(metadata['depth_reversed'])) or any(([descriptors[k]['width'], descriptors[k]['height']] != metadata['size'] for k in ('depth', 'color'))):
        raise ValueError('Unqualified frame or unregistered color and depth')
    color = plane(prefix, color_desc, '<f2', 4)[2::4, 2::4, :3].astype('f4')
    log_luma = np.log1p(np.maximum(color @ [0.2126, 0.7152, 0.0722], 0) * 80).astype('f4')
    depth = plane(prefix, descriptors['depth'], '<f4', 1)[2::4, 2::4, 0].copy()
    return (metadata, log_luma, depth if metadata['depth_reversed'] else 1 - depth)

def match_patch(source, destination, x, y, center_x, center_y, radius, patch_radius):
    """Find a normalized-correlation peak, then refine it to subpixel precision."""
    h, w = destination.shape
    if min(x, y, center_x - radius, center_y - radius) < patch_radius or x + patch_radius >= w or y + patch_radius >= h or (center_x + radius + patch_radius >= w) or (center_y + radius + patch_radius >= h):
        return None
    patch = source[y - patch_radius:y + patch_radius + 1, x - patch_radius:x + patch_radius + 1]
    patch = patch - patch.mean()
    energy = np.sum(patch * patch)
    if energy < 1e-07:
        return None
    windows = np.lib.stride_tricks.sliding_window_view(destination[center_y - radius - patch_radius:center_y + radius + patch_radius + 1, center_x - radius - patch_radius:center_x + radius + patch_radius + 1], patch.shape)
    variance = np.maximum(1e-10, np.sum(windows * windows, axis=(-2, -1)) - np.sum(windows, axis=(-2, -1)) ** 2 / patch.size)
    correlations = np.einsum('ijkl,kl->ij', windows, patch) / np.sqrt(energy * variance)
    iy, ix = np.unravel_index(np.argmax(correlations), correlations.shape)
    if correlations[iy, ix] < 0.87 or min(iy, ix) == 0 or max(iy, ix) == 2 * radius:
        return None

    def sub(a, b, c):
        return float(np.clip(0.5 * (a - c) / min(-1e-07, a - 2 * b + c), -0.5, 0.5))
    return (center_x + ix - radius + sub(*correlations[iy, ix - 1:ix + 2]), center_y + iy - radius + sub(*correlations[iy - 1:iy + 2, ix]))

def match_two_frames(first, second):
    """Match textured corners in both directions and reject nearby depth boundaries."""
    meta_before, image_before, depth_before = first
    meta_after, image_after, depth_after = second
    if image_before.shape != image_after.shape or any((meta_before.get(key) != meta_after.get(key) for key in ('pid', 'size', 'history_generation', 'depth_reversed', 'referee_boundary'))):
        return np.empty((0, 6))
    h, w = image_before.shape

    def half(g):
        rows, cols = g.shape
        g = g[:rows - rows % 2, :cols - cols % 2]
        return 0.25 * (g[::2, ::2] + g[1::2, ::2] + g[::2, 1::2] + g[1::2, 1::2])
    coarse_before, coarse_after = (half(image_before), half(image_after))
    gy, gx = np.gradient(image_before)

    def avg(z):
        return sum((np.roll(z, (i, j), (0, 1)) for i in range(-2, 3) for j in range(-2, 3))) / 25
    xx, xy, yy = (avg(gx * gx), avg(gx * gy), avg(gy * gy))
    corner_response = (xx * yy - xy * xy) / np.maximum(xx + yy, 1e-09)
    matches = []
    for y0 in range(38, h - 38, 15):
        for x0 in range(38, w - 38, 20):
            i, j = np.unravel_index(np.argmax(corner_response[y0:y0 + 10, x0:x0 + 10]), (10, 10))
            x, y = (x0 + j, y0 + i)
            if corner_response[y, x] < 2e-06:
                continue
            coarse = match_patch(coarse_before, coarse_after, x // 2, y // 2, x // 2, y // 2, 16, 3)
            if coarse is None:
                continue
            refined = match_patch(image_before, image_after, x, y, round(2 * coarse[0]), round(2 * coarse[1]), 4, 5)
            if refined is None:
                continue
            u, v = refined
            ui, vi = (round(u), round(v))
            back = match_patch(image_after, image_before, ui, vi, x, y, 4, 5)
            if back is None or np.hypot(back[0] - x, back[1] - y) > 0.85:
                continue
            depth1, depth2 = (float(depth_before[y, x]), float(depth_after[vi, ui]))
            if not (1e-08 < depth1 < 1 and 1e-08 < depth2 < 1):
                continue
            if any((np.ptp(k[max(0, j - 1):j + 2, max(0, i - 1):i + 2]) > d * 0.05 for k, i, j, d in [(depth_before, x, y, depth1), (depth_after, ui, vi, depth2)])):
                continue
            matches.append([(x + 0.625 - w / 2) / (0.5 * h), (y + 0.625 - h / 2) / (0.5 * h), depth1, (u + 0.625 - w / 2) / (0.5 * h), (v + 0.625 - h / 2) / (0.5 * h), depth2])
    return np.array(matches)

def rotation_matrix(axis_angle):
    angle = np.linalg.norm(axis_angle)
    x, y, z = axis_angle
    skew = np.array([[0, -z, y], [z, 0, -x], [-y, x, 0]])
    return np.eye(3) + np.sinc(angle / np.pi) * skew + 0.5 * np.sinc(angle / (2 * np.pi)) ** 2 * (skew @ skew)

def held_out_error(matches, focal, offset, training, depth_weight=0.6):
    """Fit six motion parameters and score the other third of the matched points."""

    def residual(params):
        rays = np.column_stack((matches[:, :2] / focal, np.ones(len(matches))))
        inverse_depth = matches[:, 2] + offset
        projected = rays @ rotation_matrix(params[:3]).T + inverse_depth[:, None] * params[3:]
        depth_scale = np.maximum(0.05, projected[:, 2])
        return np.column_stack((focal * projected[:, :2] / depth_scale[:, None] - matches[:, 3:5], depth_weight * np.log(inverse_depth / depth_scale / (matches[:, 5] + offset))))
    pose = np.zeros(6)
    for _ in range(5):
        errors = residual(pose)
        jacobian = np.stack([(residual(pose + basis * 1e-05) - errors) / 1e-05 for basis in np.eye(6)], axis=2)
        weight = np.minimum(1.0, 0.004 / np.maximum(1e-09, np.linalg.norm(errors, axis=1)))[training, None, None]
        delta = np.linalg.lstsq((jacobian[training] * weight).reshape(-1, 6), (-errors[training] * weight[:, :, 0]).ravel(), rcond=1e-09)[0]
        pose += delta
        if np.linalg.norm(delta) < 1e-07:
            break
    loss = np.linalg.norm(residual(pose)[~training], axis=1)
    return np.median(loss) * 180

def score_lenses(pairs, vertical_angles, offsets):
    losses = np.zeros((len(vertical_angles), len(offsets)))
    for index, angle in enumerate(vertical_angles):
        focal = 1 / np.tan(np.radians(angle / 2))
        for j, offset in enumerate(offsets):
            scores = [held_out_error(matches, focal, offset, np.arange(len(matches)) % 3 != 1) for matches in pairs]
            losses[index, j] = np.median(scores)
    return losses

def summary(vals):
    return [round(float(v), 5) for v in vals]

def check_synthetic_projection():
    rng = np.random.default_rng(118)
    xy = rng.uniform(-1, 1, (450, 2))
    depth = rng.uniform(0.002, 0.05, len(xy))
    focal = 1 / np.tan(np.radians(60 / 2))
    offset = 0.001
    transformed = np.column_stack((xy / focal, np.ones(len(xy)))) @ rotation_matrix([0.028, -0.045, 0.001]).T + (depth + offset)[:, None] * [0.15, 0.08, 0.09]
    observations = np.column_stack((xy, depth, focal * transformed[:, :2] / transformed[:, 2, None], (depth + offset) / transformed[:, 2] - offset))
    angles = np.arange(54.0, 67.0)
    offsets = [0, 0.0005, 0.001, 0.002]
    cost = score_lenses([observations], angles, offsets)
    i, j = np.unravel_index(np.argmin(cost), cost.shape)
    assert angles[i] == 60 and offsets[j] == offset and (cost[i, j] < 1e-07)
    print('Known synthetic 60-degree lens recovered:', angles[i], offsets[j], flush=True)
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('folders', type=Path, nargs='*')
    args = parser.parse_args()
    root = args.output
    root.mkdir(parents=True, exist_ok=True)
    folders = args.folders
    check_synthetic_projection()
    grid = np.arange(36, 76, 2.0)
    offs = [0, 5e-05, 0.0005, 0.002, 0.01, 0.1]
    for folder in folders:
        pairs = [(i, i + 1) for i in range(29)] + [(0, 2), (2, 5), (5, 8)]
        groups = []
        support = []
        for i, j in pairs:
            try:
                d = match_two_frames(read_frame(folder, i), read_frame(folder, j))
            except (ValueError, OSError, KeyError):
                continue
            if len(d) > 35:
                amplitude = np.median(np.linalg.norm(d[:, 3:5] - d[:, :2], axis=1)) * 180
                if amplitude > 0.6:
                    groups.append(d)
                support.append([i, j, len(d), round(float(amplitude), 2)])
        print(folder.name, support, flush=True)
        if len(groups) < 2:
            (root / (folder.name + '-independent-lens.json')).write_text(json.dumps(dict(capture=folder.name, state='insufficient independent image motion', support=support), indent=2))
            continue
        np.savez_compressed(root / (folder.name + '-texture-matches.npz'), *groups)
        cost = score_lenses(groups, grid, offs)
        i, j = np.unravel_index(np.argmin(cost), cost.shape)
        result = dict(capture=folder.name, grid=summary(grid), offsets=offs, support=support, scores=cost.tolist(), best_vfov=float(grid[i]), depth_offset=offs[j])
        (root / (folder.name + '-independent-lens.json')).write_text(json.dumps(result, indent=2))
        print('Best degrees, offset and held-out residual curve:', grid[i], offs[j], summary(cost.min(axis=1)), flush=True)
        fine = np.arange(max(grid[0] - 1, grid[i] - 5), min(grid[-1] + 1, grid[i] + 5) + 0.01, 0.25)
        offsets = [0, 5e-05, 0.0005, 0.002, 0.01]
        pair_scores = np.array([score_lenses([d], fine, offsets) for d in groups])
        pooled = np.median(pair_scores, axis=0)
        a, b = np.unravel_index(pooled.argmin(), pooled.shape)
        report = dict(state='offline candidate', fov=float(fine[a]), offset=offsets[b], fov_grid=fine.tolist(), offset_grid=offsets, per_pair_scores=pair_scores.tolist())
        (root / (folder.name + '-fine-lens.json')).write_text(json.dumps(report))
