"""Six original procedural materials for shared comparisons. No game assets or model weights."""
import json
import io
from pathlib import Path
import numpy as np
from PIL import Image
from .detail import Material, gradient, unit, srgb, linear, blur

CASES = [("oak_boards", "wood", 12, "development"),
         ("painted_timber", "wood", 37, "held_out"),
         ("warm_sandstone", "masonry", 51, "development"),
         ("cool_slate", "masonry", 83, "held_out"),
         ("brown_linen", "fabric", 103, "development"),
         ("olive_canvas", "fabric", 149, "held_out")]


def image(a):
    return Image.fromarray(np.round(np.clip(a, 0, 1)*255).astype(np.uint8))


def write_maps(m, folder):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    color = np.concatenate([srgb(m.color[..., :3]), m.color[..., 3:4]], -1)
    image(color).save(folder / "base_color.png")
    image(m.normal*.5+.5).save(folder / "normal.png")
    image(m.roughness).save(folder / "roughness.png")


def read_maps(folder, source_bytes=None):
    folder = Path(folder)
    data = source_bytes if source_bytes is not None else {
        role: (folder/f"{role}.png").read_bytes() for role in ("base_color", "normal", "roughness")}
    c = np.asarray(Image.open(io.BytesIO(data["base_color"])).convert("RGBA"), dtype=float)/255
    c[..., :3] = linear(c[..., :3])
    n = np.asarray(Image.open(io.BytesIO(data["normal"])).convert("RGB"), dtype=float)/127.5-1
    r = np.asarray(Image.open(io.BytesIO(data["roughness"])).convert("L"), dtype=float)/255
    return Material(c, unit(n), r)


def make(family, seed, size=256):
    y, x = np.mgrid[:size, :size] / size
    rng = np.random.default_rng(seed)
    cloud = blur(rng.standard_normal((size, size)), 14)
    cloud /= max(float(cloud.std()), 1e-8)
    if family == "wood":
        warp = .15*np.sin(2*np.pi*y) + .08*np.sin(6*np.pi*y)
        grain = np.sin(2*np.pi*(16*x + warp))*.6 + np.sin(2*np.pi*(31*x + warp*2))*.15
        joints = np.exp(-np.sin(4*np.pi*x)**2/.008)
        height = .4*grain - 2*joints + .12*cloud
        tint = np.array([.23, .12, .055]) if seed == 12 else np.array([.16, .21, .18])
        rgb = tint + (.025*grain-.055*joints+.005*cloud)[..., None]
        rough = .62+.06*grain+.03*joints
    elif family == "masonry":
        joints = np.maximum(np.exp(-np.sin(4*np.pi*y)**2/.018),
                            np.exp(-np.sin(4*np.pi*x+.4*np.sin(2*np.pi*y))**2/.018))
        height = .55*cloud-1.8*joints
        tint = np.array([.28, .20, .12]) if seed == 51 else np.array([.13, .15, .18])
        rgb = tint + (.018*cloud-.045*joints)[..., None]
        rough = .7+.04*cloud+.08*joints
    else:
        weave = np.sin(2*np.pi*16*x)*np.cos(2*np.pi*16*y)
        folds = np.sin(2*np.pi*(x+y))*.5
        height = .38*weave+.15*cloud+folds
        # Brown linen deliberately overlaps wood colour: family is explicit, not guessed by hue.
        tint = np.array([.23, .12, .055]) if seed == 103 else np.array([.13, .16, .075])
        rgb = tint + (.012*weave+.01*folds+.004*cloud)[..., None]
        rough = .79+.03*weave
    dx, dy = gradient(height)
    n = unit(np.stack([-dx*.5, -dy*.5, np.ones_like(dx)], -1))
    return Material(np.concatenate([np.clip(rgb, 0, 1), np.ones((*x.shape, 1))], -1),
                    n, np.clip(rough, .1, 1))


def create(folder, size=256):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    records = []
    for name, family, seed, split in CASES:
        target = folder/name
        write_maps(make(family, seed, size), target)
        source = dict(material_id=name, family=family, seed=seed, split=split,
                      fixture=True, address="wrap", source="original-procedural-fixture-v1",
                      license="CC0-1.0", normal_convention="tangent_xyz_positive_z",
                      roughness_convention="perceptual_roughness", size=size)
        (target/"source.json").write_text(json.dumps(source, indent=2)+"\n", encoding="utf-8")
        records.append(source)
    (folder/"corpus.json").write_text(json.dumps(records, indent=2)+"\n", encoding="utf-8")
    return records
