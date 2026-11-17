import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image

from tools.material_lab import cache, dds
from tools.material_lab.detail import Material, linear, mip_chain, unit

PAD = 16  # source texels wrapped around each side before the upscale


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def upscale_rgb(rgb, scale, upscaler, model, work):
    """rgb: uint8 HxWx3 sRGB, periodic. Returns uint8 (H*scale)x(W*scale)x3."""
    h, w = rgb.shape[:2]
    padded = np.pad(rgb, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="wrap")
    if upscaler == "lanczos":
        big = Image.fromarray(padded).resize((padded.shape[1]*scale, padded.shape[0]*scale), Image.LANCZOS)
    else:
        src, dst = work/"in.png", work/"out.png"
        Image.fromarray(padded).save(src)
        run = subprocess.run([str(upscaler), "-i", str(src), "-o", str(dst), "-n", model, "-s", str(scale)],
                             capture_output=True, text=True, timeout=900, cwd=str(Path(upscaler).parent))
        if run.returncode or not dst.exists():
            raise RuntimeError(f"upscaler failed ({run.returncode}): {run.stdout[-2000:]} {run.stderr[-2000:]}")
        big = Image.open(dst).convert("RGB")
        if big.size != (padded.shape[1]*scale, padded.shape[0]*scale):
            raise ValueError(f"upscaler returned {big.size}, expected {(padded.shape[1]*scale, padded.shape[0]*scale)}")
    out = np.asarray(big, dtype=np.uint8)[PAD*scale:(PAD+h)*scale, PAD*scale:(PAD+w)*scale]
    if out.shape != (h*scale, w*scale, 3):
        raise ValueError("cropped upscale has the wrong extent")
    return out


def prepare(source_folder, cache_root, texconv, upscaler, model, scale, tool_identity, upscale=None):
    source_folder, cache_root, texconv = Path(source_folder), Path(cache_root), Path(texconv)
    metadata = json.loads((source_folder/"source.json").read_text(encoding="utf-8"))
    if metadata.get("address") != "wrap":
        raise ValueError("baseline v1 handles declared periodic sources only")
    raw = (source_folder/"base_color.png").read_bytes()  # the identified bytes are the decoded bytes
    rgba = np.asarray(Image.open(io.BytesIO(raw)).convert("RGBA"), dtype=np.uint8)
    h, w = rgba.shape[:2]
    # Exactly the source description the lab writes for the same file, so the comparison accepts it.
    source = dict(content_sha256=sha256(raw), identity_domain="file-bytes", width=w, height=h, first_mip=0,
                  mip_count=1, view_format=29, complete_observed_range=True, resource_format=0,
                  first_array_slice=0, array_size=1)
    entry = dict(id="base_color", role="base_color", source=source,
                 channels=["color_r", "color_g", "color_b", "opacity"], encoding="srgb", address="wrap")
    recipe = dict(schema_version=1, material_id=metadata["material_id"],
                  algorithm=tool_identity["algorithm"], algorithm_version=tool_identity["version"],
                  settings=dict(scale=scale, model=model, wrap_pad=PAD, maps="base_color only",
                                upscaler_sha256=tool_identity["upscaler_sha256"],
                                model_sha256=tool_identity["model_sha256"]),
                  seed=metadata["seed"], fixture=bool(metadata["fixture"]), sources=[entry],
                  encoder_sha256=sha256(texconv.read_bytes()), numpy_version=np.__version__)
    if tool_identity.get("inference"):
        recipe["settings"]["inference"] = tool_identity["inference"]
    key = sha256(cache.canonical(recipe))
    target = cache_root/key
    if target.exists():
        return target, cache.validate(target), True
    cache_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".preparing-", dir=cache_root) as tmp:
        folder = Path(tmp)
        big = (upscale or upscale_rgb)(rgba[..., :3], scale, upscaler, model, folder)
        alpha = np.asarray(Image.fromarray(rgba[..., 3]).resize((w*scale, h*scale), Image.BICUBIC), dtype=float)/255
        for leftover in ("in.png", "out.png"):
            if (folder/leftover).exists():
                (folder/leftover).unlink()
        colour = np.concatenate([linear(big.astype(float)/255), alpha[..., None]], -1)
        # Flat stand-ins only so the lab's chain builder accepts the set; they are never written.
        flat_normal = unit(np.dstack([np.zeros((h*scale, w*scale)), np.zeros((h*scale, w*scale)), np.ones((h*scale, w*scale))]))
        chain = mip_chain(Material(np.clip(colour, 0, 1), flat_normal, np.full((h*scale, w*scale), .5)))
        data = dds.compress(chain, "base_color", folder/"base_color.dds", texconv)
        fmt, levels = dds.layout(data)
        maps = [dict(**entry, dxgi_format=fmt, width=w*scale, height=h*scale, file="base_color.dds",
                     sha256=sha256(data), mips=levels)]
        record = {k: recipe[k] for k in ("schema_version", "material_id", "algorithm", "algorithm_version", "settings",
                                         "seed", "fixture")}
        record.update(entry_key=key, recipe=recipe, maps=maps, payload_bytes=sum(m["byte_size"] for m in levels))
        (folder/"manifest.json").write_text(json.dumps(record, indent=2, allow_nan=False)+"\n", encoding="utf-8")
        cache.validate(folder)
        try:
            os.rename(folder, target)
        except OSError:
            if not target.exists():
                raise
            cache.validate(target)
    return target, cache.validate(target), False


def main():
    p = argparse.ArgumentParser(description="Plain colour-upscale baseline entries for the material comparison")
    p.add_argument("--sources", type=Path, required=True, help="folder of <material_id>/{base_color.png,source.json}")
    p.add_argument("--cache", type=Path, required=True)
    p.add_argument("--index", type=Path, required=True, help="JSON written for material_lab demo --baseline-index")
    p.add_argument("--texconv", type=Path, required=True)
    p.add_argument("--upscaler", required=True, help="path to realesrgan-ncnn-vulkan.exe, or 'lanczos'")
    p.add_argument("--model", default="realesrgan-x4plus")
    p.add_argument("--scale", type=int, default=4, choices=(2, 4))
    p.add_argument("--version", default="unstated", help="the upscaler's release tag")
    args = p.parse_args()
    if args.upscaler == "lanczos":
        identity = dict(algorithm="pillow-lanczos-colour-upscale", version=Image.__version__, upscaler_sha256="", model_sha256="")
        args.model = "lanczos"
    else:
        exe = Path(args.upscaler)
        models = sorted((exe.parent/"models").glob(args.model+".*"))
        if not exe.is_file() or not models:
            raise SystemExit("upscaler program or its model files not found")
        identity = dict(algorithm="realesrgan-ncnn-vulkan-colour-upscale", version=args.version,
                        upscaler_sha256=sha256(exe.read_bytes()),
                        model_sha256=sha256(b"".join(sha256(m.read_bytes()).encode("ascii") for m in models)))
    index = {}
    for folder in sorted(d for d in args.sources.iterdir() if (d/"source.json").is_file()):
        target, record, hit = prepare(folder, args.cache, args.texconv, args.upscaler, args.model, args.scale, identity)
        index[record["material_id"]] = str(target.resolve()).replace("\\", "/")
        m = record["maps"][0]
        print(f"{record['material_id']}: {m['width']}x{m['height']}, {record['payload_bytes']} bytes, "
              f"{'cache hit' if hit else 'prepared'}", flush=True)
    args.index.parent.mkdir(parents=True, exist_ok=True)
    args.index.write_text(json.dumps(index, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(dict(index=str(args.index.resolve()), entries=len(index))))


if __name__ == "__main__":
    main()
