"""Offline reference renders from decoded BC payloads; no game or browser needed.

This is an appearance comparison, not a substitute for the game's shader or
measurement of runtime speed. All panels share the light, geometry and exposure.
"""
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from . import cache, viewer
from .detail import linear, unit


def maps(folder, level=0):
    record = cache.validate(folder)
    result = {}
    for m in record["maps"]:
        images = list(viewer.decoded_mips(folder, m))
        decoded = []
        for image in images if level is None else [images[min(level, len(images)-1)]]:
            pixels = np.asarray(image, dtype=np.float64)/255
            if m["role"] == "base_color":
                pixels[..., :3] = linear(pixels[..., :3])
            decoded.append(pixels)
        result[m["role"]] = decoded if level is None else decoded[0]
    return result


def sample(a, uv):
    h, w = a.shape[:2]
    xy = uv*np.array([w, h])-.5
    base = np.floor(xy).astype(np.int64)
    fraction = xy-base
    x, y = base[..., 0], base[..., 1]
    dx, dy = fraction[..., 0:1], fraction[..., 1:2]
    return ((a[y % h, x % w]*(1-dx) + a[y % h, (x+1) % w]*dx)*(1-dy) +
            (a[(y+1) % h, x % w]*(1-dx) + a[(y+1) % h, (x+1) % w]*dx)*dy)


def sample_mips(levels, uv):
    """Trilinear reference sampling at the actual UV footprint for EACH map.

    A 4x colour upscale selects two mips deeper at the same footprint; original
    normal/roughness maps keep their own LOD. No forced shared integer mip.
    """
    if isinstance(levels, np.ndarray):
        return sample(levels, uv)
    h, w = levels[0].shape[:2]
    dy, dx = np.gradient(uv, axis=(0, 1))
    extent = np.array([w, h])
    rho = np.maximum(np.linalg.norm(dx*extent, axis=-1), np.linalg.norm(dy*extent, axis=-1))
    lod = np.clip(np.log2(np.maximum(rho, 1e-12)), 0, len(levels)-1)
    low = np.floor(lod).astype(int)
    high = np.minimum(low+1, len(levels)-1)
    fraction = lod-low
    result = np.zeros((*uv.shape[:-1], levels[0].shape[-1]))
    for index, pixels in enumerate(levels):
        weight = (low == index)*(1-fraction) + (high == index)*fraction
        mask = weight > 0
        if mask.any():
            result[mask] += sample(pixels, uv[mask])*weight[mask, None]
    return result


def render(material, size=384, light_phase=1.35, yaw_degrees=-18):
    grid = (np.arange(size)+.5)/size
    u, v = np.meshgrid(grid, 1-grid)
    p = np.stack([(u-.5)*2.35, (v-.5)*2.35, np.full_like(u, -3)], -1)
    ray = unit(p)
    yaw = np.deg2rad(yaw_degrees)
    n0 = np.array([np.sin(yaw), 0, np.cos(yaw)])
    tangent = np.array([np.cos(yaw), 0, -np.sin(yaw)])
    bitangent = np.array([0, 1, 0])
    t = -3*np.cos(yaw)/(ray @ n0)
    point = np.array([0, 0, 3]) + ray*t[..., None]
    q = np.stack([point @ tangent, point[..., 1]], -1)
    inside = np.max(np.abs(q), -1) <= 1
    st = q*.5+.5
    st[..., 1] = 1-st[..., 1]
    albedo = sample_mips(material["base_color"], st)[..., :3]
    xy = sample_mips(material["normal"], st)[..., :2]*2-1
    z = np.sqrt(np.maximum(.001, 1-np.sum(xy*xy, -1)))
    normal = unit(tangent*xy[..., 0:1] - bitangent*xy[..., 1:2] + n0*z[..., None])
    rough = np.maximum(.06, sample_mips(material["roughness"], st)[..., 0])
    view = unit(np.array([0, 0, 3])-point)
    light = unit(np.array([1.7*np.cos(light_phase), 1., 1.25+.65*np.sin(light_phase)]))
    half = unit(view+light)
    nl = np.maximum(0, normal @ light)
    nv = np.maximum(.001, np.sum(normal*view, -1))
    nh = np.maximum(0, np.sum(normal*half, -1))
    vh = np.maximum(0, np.sum(view*half, -1))
    a2 = rough**4
    d = nh*nh*(a2-1)+1
    distribution = a2/(np.pi*d*d)
    k = (rough+1)**2/8
    geometry = nv/(nv*(1-k)+k)*nl/(nl*(1-k)+k)
    fresnel = .04+.96*(1-vh)**5
    specular = distribution*geometry*fresnel/np.maximum(.001, 4*nv*np.maximum(nl,.001))
    brdf = (1-fresnel[..., None])*albedo/np.pi + specular[..., None]
    color = albedo*.2+brdf*nl[..., None]*3.5
    color = np.maximum(0, color/(1+color))**(1/2.2)
    color[~inside] = [.065,.09,.105]
    return Image.fromarray(np.round(np.clip(color, 0, 1)*255).astype(np.uint8))


def font(size):
    for name in ("C:/Windows/Fonts/segoeui.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


def previews(cases, output, size=384):
    output = Path(output)
    for case in cases:
        original, coordinated = maps(case["original"], None), maps(case["coordinated"], None)
        baseline = {**original, **maps(case["baseline"], None)} if case.get("baseline") else {
            **original, "base_color": coordinated["base_color"]}
        normal_only = {**original, "normal": coordinated["normal"]}
        width, padding = size*3+64, 16
        sheet = Image.new("RGB", (width, size*2+270), "#11171b")
        draw = ImageDraw.Draw(sheet)
        draw.text((24, 16), "NeuralAlias | Material Lab", font=font(26), fill="#8dd8bc")
        draw.text((24, 54), case["name"].replace("_", " ").title(), font=font(22), fill="#edf3f4")
        draw.text((24, 86), "Identical light, camera and exposure. Rendered from the compressed cache.",
                  font=font(16), fill="#a8b9c1")
        entries = [("Original", original),
                   ("AI colour upscale" if case.get("baseline") else "Colour-only control", baseline),
                   ("Coordinated material", coordinated),
                   ("Original | grazing light", original),
                   ("New normals only | grazing light", normal_only),
                   ("Coordinated | grazing light", coordinated)]
        for i, (label, material) in enumerate(entries):
            col, row = i % 3, i // 3
            x, y = padding+col*(size+16), 122+row*(size+56)
            draw.text((x, y), label, font=font(16), fill="#edf3f4")
            sheet.paste(render(material, size, 1.35 if row == 0 else .15), (x, y+28))
        draw.text((24, size*2+244), "Procedural candidate / synthetic source / no in-game result claimed",
                  font=font(14), fill="#a8b9c1")
        sheet.save(output/f"Preview-{case['name']}.png")
