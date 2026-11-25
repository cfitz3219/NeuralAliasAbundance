"""Offline composition of a wider AO field over an already qualified contact layer."""
import numpy as np

from form_lighting import broad, tile_means


def world_addition(base, wide, light, depth, projection_ab, valid, shadow_guard,
                   mean_cap=0.24, tile_cap=0.82):
    """Return combined stops, the protected foreground mask, and the world gain.

    Both input fields are nonpositive stops from the same frame. The caller
    supplies exposure-adjusted linear luminance and a baseline already within
    budget. Projection coefficients satisfy device depth = a + b / distance.
    Foreground protection uses relative depth and contains no subject classifier.
    """
    a, b = projection_ab
    with np.errstate(over="ignore", divide="ignore", invalid="ignore"):
        distance = b / (depth - a)
    usable = valid & np.isfinite(distance) & (distance > 0) & np.isfinite(wide)
    if not usable.any():
        return base.copy(), np.zeros_like(valid), 0.0

    near_distance = np.quantile(distance[usable], 0.05)
    blend = np.clip((distance / near_distance - 1.35) / 0.45, 0, 1)
    blend = np.where(usable, blend * blend * (3 - 2 * blend), 0).astype("f4")

    # Dilate the protected region onto the far side of silhouette edges.
    # Missing depth has no geometric evidence to extend this protection.
    radius = max(1, round(depth.shape[0] * 0.004))
    edge_weight = np.where(usable, blend, 1).astype("f4")
    for axis in (0, 1):
        padding = ((radius, radius), (0, 0)) if axis == 0 else ((0, 0), (radius, radius))
        padded = np.pad(edge_weight, padding, mode="edge")
        length = depth.shape[axis]
        bands = [padded[i:i + length, :] if axis == 0 else padded[:, i:i + length]
                 for i in range(2 * radius + 1)]
        edge_weight = np.minimum.reduce(bands)
    blend = np.where(usable, edge_weight, 0)

    # Only the difference is eligible. Each contact is counted once.
    extra = np.maximum(base - wide, 0) * blend
    after_base = light * np.exp2(base)
    floor = max(0.005, float(np.quantile(after_base[valid], 0.10)))
    room = np.maximum(0, np.log2(np.maximum(after_base, 1e-10) / floor))
    extra = np.where(usable, np.minimum(extra, room * 0.98), 0)

    spent, side = tile_means(-base, valid)
    requested, _ = tile_means(extra, valid)
    remaining = np.maximum(tile_cap - spent, 0)
    scale = np.minimum(1, remaining / np.maximum(requested, 1e-12))

    def per_pixel(tiles):
        expanded = tiles.repeat(side, axis=0).repeat(side, axis=1)
        return expanded[:depth.shape[0], :depth.shape[1]].astype("f4")

    existing_guard = np.where(np.isfinite(shadow_guard), np.clip(shadow_guard, 0, 1), 0)
    extra *= 1 - (1 - per_pixel(scale)) * (1 - existing_guard)
    requested, _ = tile_means(extra, valid)
    extra *= per_pixel(np.minimum(1, remaining / np.maximum(requested, 1e-12)))
    mean_remaining = max(0, mean_cap + float(base[valid].mean()))
    extra *= min(1, mean_remaining / max(1e-12, float(extra[valid].mean())))

    scene_light = light[valid]
    base_scene = base[valid]
    extra_scene = extra[valid]
    original_span = broad(scene_light)

    def contrast(gain):
        return broad(scene_light * np.exp2(base_scene - gain * extra_scene)) / original_span

    gain = 1.0
    if not 0.92 <= contrast(gain) <= 1.10:
        low, high = 0.0, 1.0
        for _ in range(13):
            mid = (low + high) * 0.5
            if 0.92 <= contrast(mid) <= 1.10:
                low = mid
            else:
                high = mid
        gain = low
    return base - gain * extra, usable & (blend == 0), gain
