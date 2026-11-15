"""Content-addressed, immutable preparation cache. No runtime dependency."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import numpy as np
from . import dds
from .detail import ALGORITHM, VERSION, enhance, mip_chain
from .corpus import read_maps


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def validate(folder):
    folder = Path(folder)
    record = json.loads((folder/"manifest.json").read_text(encoding="utf-8"))
    if record["schema_version"] != 1 or digest(canonical(record["recipe"])) != record["entry_key"]:
        raise ValueError("material recipe identity mismatch")
    for field in ("material_id", "algorithm", "algorithm_version", "settings", "seed", "fixture"):
        if record[field] != record["recipe"][field]:
            raise ValueError(f"manifest differs from recipe: {field}")
    names, roles, extents, size = set(), set(), set(), 0
    expected_sources = {s["id"]: s for s in record["recipe"]["sources"]}
    if len(expected_sources) != len(record["recipe"]["sources"]):
        raise ValueError("duplicate source identity in recipe")
    for m in record["maps"]:
        if Path(m["file"]).name != m["file"] or ":" in m["file"] or "\\" in m["file"] or m["file"] in names:
            raise ValueError("map path is not a unique DDS leaf")
        names.add(m["file"])
        if m["role"] not in dds.FORMATS or m["role"] in roles:
            raise ValueError("unsupported or duplicate map role")
        roles.add(m["role"])
        declared = expected_sources.get(m["id"])
        if not declared or m["source"] != declared["source"] or m["role"] != declared["role"]:
            raise ValueError("map source/role does not match recipe")
        for field in ("channels", "encoding", "address"):
            if m[field] != declared[field]:
                raise ValueError(f"map interpretation differs: {field}")
        raw = (folder/m["file"]).read_bytes()
        if digest(raw) != m["sha256"]:
            raise ValueError("prepared map digest mismatch")
        fmt, levels = dds.layout(raw)
        if fmt != m["dxgi_format"] or fmt != dds.FORMATS[m["role"]][0] or levels != m["mips"]:
            raise ValueError("prepared map layout mismatch")
        if levels[0]["width"] != m["width"] or levels[0]["height"] != m["height"]:
            raise ValueError("prepared map dimensions mismatch")
        if levels[-1]["width"] != 1 or levels[-1]["height"] != 1:
            raise ValueError("prepared map is missing coarse mips")
        extents.add((m["width"], m["height"]))
        size += len(raw)-148
    if len(names) != len(expected_sources) or len(extents) != 1 or size != record["payload_bytes"]:
        raise ValueError("incomplete material set or incorrect payload accounting")
    return record


def prepare(source_folder, cache_root, texconv, strength=1., candidate="coordinated"):
    source_folder, cache_root, texconv = map(Path, (source_folder, cache_root, texconv))
    metadata = json.loads((source_folder/"source.json").read_text(encoding="utf-8"))
    if metadata.get("address") != "wrap" or metadata.get("normal_convention") != "tangent_xyz_positive_z" or metadata.get("roughness_convention") != "perceptual_roughness":
        raise ValueError("preparer v1 supports declared periodic XYZ normal/perceptual roughness only")
    if candidate not in ("coordinated", "original"):
        raise ValueError("unknown candidate")
    # Decode and identify the same owned bytes, even if a capture writer updates a
    # source file while generation is running. Do not hash a later file version.
    source_bytes = {role: (source_folder/f"{role}.png").read_bytes() for role in dds.FORMATS}
    original = read_maps(source_folder, source_bytes)
    material = enhance(original, metadata["family"], metadata["seed"], strength if candidate == "coordinated" else 0)
    h, w = material.roughness.shape
    descriptions = {
        "base_color": (["color_r", "color_g", "color_b", "opacity"], "srgb", 29),
        "normal": (["normal_x", "normal_y", "unused", "unused"], "tangent_xy_unorm_positive_z", 28),
        "roughness": (["roughness", "unused", "unused", "unused"], "perceptual_roughness", 61)}
    sources = []
    for role, (channels, encoding, source_fmt) in descriptions.items():
        source = dict(content_sha256=digest(source_bytes[role]),
                      identity_domain="file-bytes", width=w, height=h, first_mip=0, mip_count=1,
                      view_format=source_fmt, complete_observed_range=True,
                      resource_format=0, first_array_slice=0, array_size=1)
        sources.append(dict(id=role, role=role, source=source, channels=channels, encoding=encoding, address="wrap"))
    recipe = dict(schema_version=1, material_id=metadata["material_id"],
                  algorithm=ALGORITHM if candidate == "coordinated" else "na-original-repack", algorithm_version=VERSION,
                  settings=dict(strength=strength if candidate == "coordinated" else 0, family=metadata["family"]),
                  seed=metadata["seed"], fixture=bool(metadata["fixture"]), sources=sources,
                  encoder_sha256=digest(texconv.read_bytes()), numpy_version=np.__version__)
    key = digest(canonical(recipe))
    target = cache_root/key
    if target.exists():
        return target, validate(target), True
    cache_root.mkdir(parents=True, exist_ok=True)
    chain = mip_chain(material)
    # Same-filesystem rename publishes only complete entries. Temp cleanup is owned by this context.
    with tempfile.TemporaryDirectory(prefix=".preparing-", dir=cache_root) as tmp:
        folder = Path(tmp)
        maps = []
        for source in sources:
            role = source["role"]
            raw = dds.compress(chain, role, folder/f"{role}.dds", texconv)
            fmt, levels = dds.layout(raw)
            maps.append(dict(**source, dxgi_format=fmt, width=w, height=h, file=f"{role}.dds",
                             sha256=digest(raw), mips=levels))
        record = {k: recipe[k] for k in ("schema_version", "material_id", "algorithm", "algorithm_version", "settings", "seed", "fixture")}
        record.update(entry_key=key, recipe=recipe, maps=maps,
                      payload_bytes=sum(sum(m["byte_size"] for m in out["mips"]) for out in maps))
        (folder/"manifest.json").write_text(json.dumps(record, indent=2, allow_nan=False)+"\n", encoding="utf-8")
        validate(folder)
        try:
            os.rename(folder, target)
        except OSError:
            if not target.exists():
                raise
            # A concurrent preparer may publish the same immutable recipe first.
            validate(target)
    return target, validate(target), False
