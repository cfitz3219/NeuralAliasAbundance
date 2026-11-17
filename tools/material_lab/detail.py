"""Deterministic guided-detail candidate, not an AI-trained model.

One material-space field drives colour, normal and roughness residuals.
Generation is offline and does not alter texture dimensions or UV coordinates.
"""
from dataclasses import dataclass
import numpy as np

ALGORITHM = "na-guided-material-detail"
VERSION = "1"


def linear(s):
    return np.where(s <= .04045, s / 12.92, ((s + .055) / 1.055) ** 2.4)


def srgb(x):
    x = np.maximum(x, 0)
    return np.where(x <= .0031308, x * 12.92, 1.055 * x ** (1 / 2.4) - .055)


def unit(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-12)


def blur(a, rounds=1):
    # Periodic five-tap separable filter. Only periodic inputs use this preparer.
    for _ in range(rounds):
        for axis in (0, 1):
            a = (np.roll(a, 2, axis) + 4*np.roll(a, 1, axis) + 6*a +
                 4*np.roll(a, -1, axis) + np.roll(a, -2, axis)) / 16
    return a


def gradient(a):
    return ((np.roll(a, -1, 1)-np.roll(a, 1, 1))*.5,
            (np.roll(a, -1, 0)-np.roll(a, 1, 0))*.5)


@dataclass
class Material:
    color: np.ndarray  # H,W,4 linear RGB + unchanged opacity
    normal: np.ndarray  # H,W,3 tangent unit vector, positive Z
    roughness: np.ndarray  # H,W perceptual roughness

    def copy(self):
        return Material(self.color.copy(), self.normal.copy(), self.roughness.copy())


def check(m):
    h, w = m.roughness.shape
    if h < 8 or w < 8 or h & (h-1) or w & (w-1):
        raise ValueError("v1 requires power-of-two periodic textures, at least 8x8")
    if m.color.shape != (h, w, 4) or m.normal.shape != (h, w, 3):
        raise ValueError("material map extents differ")
    if not all(np.isfinite(a).all() for a in (m.color, m.normal, m.roughness)):
        raise ValueError("non-finite source material")
    if np.min(m.color) < 0 or np.max(m.color) > 1 or np.min(m.roughness) < 0 or np.max(m.roughness) > 1:
        raise ValueError("v1 material values must be normalized")
    if np.min(m.normal[..., 2]) <= 0 or np.max(np.abs(np.linalg.norm(m.normal, axis=-1)-1)) > .015:
        raise ValueError("v1 expects normalized positive-Z tangent normals")


def enhance(source, family, seed=0, strength=1., mask=None):
    check(source)
    if family not in ("wood", "masonry", "fabric"):
        raise ValueError("choose an explicit material family; classification is not implemented")
    if not np.isfinite(strength) or not 0 <= strength <= 1:
        raise ValueError("strength must be finite and in [0,1]")
    if strength == 0:
        return source.copy()
    h, w = source.roughness.shape
    if mask is None:
        mask = np.ones((h, w))
    if mask.shape != (h, w) or not np.isfinite(mask).all() or np.min(mask) < 0 or np.max(mask) > 1:
        raise ValueError("invalid protected-region mask")
    lum = source.color[..., :3] @ np.array([.2126, .7152, .0722])
    gx, gy = gradient(blur(lum, 2))
    covariance = np.array([[np.mean(gx*gx), np.mean(gx*gy)], [np.mean(gx*gy), np.mean(gy*gy)]])
    _, axes = np.linalg.eigh(covariance)
    across = axes[:, 1]  # texture changes fastest across the source's dominant grain
    rng = np.random.default_rng(seed)
    noise = rng.standard_normal((h, w))
    fx = np.fft.fftfreq(w)[None, :]
    fy = np.fft.fftfreq(h)[:, None]
    radius = np.sqrt(fx*fx + fy*fy)
    band = np.exp(-((radius-.15)/.07)**2) * (radius > .04)
    if family == "wood":
        along = -across[1]*fx + across[0]*fy
        band *= .08 + .92*np.exp(-(along/.025)**2)
    elif family == "fabric":
        # Two crossing fibre directions; the shared field avoids unrelated colour/normal grain.
        band *= .1 + np.exp(-(fx/.025)**2) + np.exp(-(fy/.025)**2)
    field = np.fft.ifft2(np.fft.fft2(noise)*band).real
    field /= max(float(np.std(field)), 1e-8)
    field = np.tanh(field*.65)
    # Keep high-contrast authored seams/lettering and explicit protected regions intact.
    edge_x, edge_y = gradient(lum)
    protection = mask / (1 + 120*(edge_x*edge_x + edge_y*edge_y))
    field = (field-blur(field, 2))*protection
    dx, dy = gradient(field)
    rms = max(float(np.sqrt(np.mean(dx*dx+dy*dy))), 1e-6)
    slope = np.stack([-dx, -dy], -1) / rms * (.14*strength)
    slope = np.clip(slope, -.32, .32)
    # Add slopes in the same tangent frame; no global camera movement is synthesized.
    n = source.normal
    xy = n[..., :2]/np.maximum(n[..., 2:3], .1) + slope
    out_normal = unit(np.concatenate([xy, np.ones((h, w, 1))], -1))
    color = source.color.copy()
    color[..., :3] = np.clip(color[..., :3] + (.022*strength*field)[..., None], 0, 1)
    roughness = np.clip(source.roughness-.045*strength*field, .04, 1)
    # Exact preservation inside a supplied mask, including normal derivatives at its boundary.
    protected = mask == 0
    color[protected] = source.color[protected]
    out_normal[protected] = source.normal[protected]
    roughness[protected] = source.roughness[protected]
    return Material(color, out_normal, roughness)


def down2(a):
    # Handles rectangular power-of-two textures down through a one-pixel dimension.
    h, w = a.shape[:2]
    if h > 1:
        a = (a[0::2] + a[1::2])*.5
    if w > 1:
        a = (a[:, 0::2] + a[:, 1::2])*.5
    return a


def mip_chain(source):
    """Filter linear colour and normal first moments; carry unresolved slope variance.

    Uses a moment-based roughness approximation for this explicit fixture convention.
    Runtime packing/shader conventions still require Fable's material interpretation.
    """
    check(source)
    result = [source]
    colour = source.color.copy()
    normal_moment = source.normal.copy()
    # Mean GGX alpha^2 approximation. Do not repeatedly add variance at every level.
    alpha2_moment = source.roughness**4
    while colour.shape[:2] != (1, 1):
        colour = down2(colour)
        normal_moment = down2(normal_moment)
        alpha2_moment = down2(alpha2_moment)
        length = np.linalg.norm(normal_moment, axis=-1)
        variance = np.maximum(0, 1-length)
        rough = np.clip(alpha2_moment + variance, 0, 1)**.25
        result.append(Material(colour.copy(), unit(normal_moment), rough))
    return result
