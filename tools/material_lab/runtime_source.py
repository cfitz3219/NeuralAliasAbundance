"""Offline intake of immutable texture-subresources-v1 captures.

Uses Fable's canonical serializer. Capture and material-role qualification remain
the runtime owner's job; this module never discovers roles from filenames/colour.
"""
import io
import json
import struct
from pathlib import Path
import numpy as np
from PIL import Image
from tools.material_baseline import texsub
from . import dds
from .detail import Material, linear, unit

FIELDS = ("resource_format", "view_format", "width", "height", "first_mip",
          "mip_count", "first_array_slice", "array_size")
SRGB = {29, 91, 93, 72, 75, 78, 99}
COLOR = {28, 29, 87, 88, 91, 93, 71, 72, 74, 75, 77, 78, 98, 99}


def child(folder, name):
    folder = Path(folder).resolve()
    path = (folder/name).resolve()
    if not path.is_relative_to(folder) or path == folder:
        raise ValueError("capture files must remain within their capture folder")
    return path


def load_dump_entry(index_path, content_sha256):
    """Read Fable's actual 4525b4e index.jsonl + tightly packed DX10 DDS output.

    Filename, bind slot and candidate class never stand in for source identity.
    Association/packing is supplied separately in source.json.
    """
    index_path = Path(index_path)
    entries = [json.loads(line) for line in index_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    record = next((r for r in entries if r.get("content_sha256") == content_sha256), None)
    if record is None or record.get("identity_domain") != "texture-subresources-v1":
        raise ValueError("source digest not found in runtime dump index")
    if record.get("complete_observed_range", True) is not True:
        raise ValueError("capture is not complete for its declared observed range")
    values = [record[k] for k in FIELDS]
    validate_header(record)
    raw = child(index_path.parent, record["file"]).read_bytes()
    if len(raw) < 148 or raw[:4] != b"DDS " or raw[84:88] != b"DX10":
        raise ValueError("capture requires a DX10 DDS header")
    words = struct.unpack_from("<31I", raw, 4)
    fmt, dim, misc, arrays, _ = struct.unpack_from("<5I", raw, 128)
    first = record["first_mip"]
    w, h = max(1, record["width"] >> first), max(1, record["height"] >> first)
    if (words[0], words[3], words[2], words[6], fmt, dim, misc, arrays) != (
            124, w, h, record["mip_count"], record["view_format"], 3, 0, 1):
        raise ValueError("DDS layout differs from the runtime source observation")
    levels, offset = [], 148
    for mip in range(first, first+record["mip_count"]):
        row_bytes, rows = row_layout(fmt, w, h)
        end = offset+row_bytes*rows
        if end > len(raw):
            raise ValueError("truncated runtime DDS capture")
        levels.append((record["first_array_slice"], mip, raw[offset:end]))
        offset = end
        w, h = max(1, w//2), max(1, h//2)
    if offset != len(raw) or record.get("bytes", offset-148) != offset-148:
        raise ValueError("runtime DDS payload length differs")
    actual = texsub.digest(*values, levels)
    if actual != content_sha256:
        raise ValueError("runtime capture content digest mismatch")
    source = {k: record[k] for k in FIELDS}
    source.update(content_sha256=actual, identity_domain="texture-subresources-v1",
                  complete_observed_range=True)
    return source, levels[0][2]


def validate_header(record):
    values = [record[k] for k in FIELDS]
    if any(type(v) is not int or not 0 <= v <= 0xffffffff for v in values):
        raise ValueError("capture header requires uint32 integers")
    w, h = record["width"], record["height"]
    if not 1 <= w <= 16384 or not 1 <= h <= 16384 or record["array_size"] != 1:
        raise ValueError("preparer supports one 2D slice up to 16384 texels")
    if record["mip_count"] < 1 or record["first_mip"]+record["mip_count"] > max(w, h).bit_length():
        raise ValueError("capture mip range exceeds the resource")


def row_layout(fmt, w, h):
    if fmt in texsub.BLOCK:
        return (w+3)//4*texsub.BLOCK[fmt], (h+3)//4
    if fmt in texsub.TEXEL:
        return w*texsub.TEXEL[fmt], h
    raise ValueError("format has no v1 identity")


def load_capture(path):
    """Hash and decode from one owned byte snapshot, excluding row padding."""
    path = Path(path)
    record = json.loads(path.read_text(encoding="utf-8"))
    if record.get("identity_domain") != "texture-subresources-v1":
        raise ValueError("unsupported capture identity domain")
    values = [record[k] for k in FIELDS]
    validate_header(record)
    fmt, w, h = record["view_format"], record["width"], record["height"]
    first, count = record["first_mip"], record["mip_count"]
    if type(record.get("complete_observed_range")) is not bool:
        raise ValueError("capture must declare observed-range completeness")
    if not record["complete_observed_range"]:
        raise ValueError("capture is not complete for its declared observed range")
    levels = []
    for index, level in enumerate(record["levels"]):
        mip = first+index
        if (level["slice"], level["mip"]) != (record["first_array_slice"], mip) or index >= count:
            raise ValueError("capture levels missing or out of order")
        lw, lh = max(1, w >> mip), max(1, h >> mip)
        row_bytes, rows = row_layout(fmt, lw, lh)
        pitch = level["row_pitch"]
        if type(pitch) is not int or pitch < row_bytes:
            raise ValueError("invalid capture row pitch")
        raw = child(path.parent, level["file"]).read_bytes()
        if len(raw) != pitch*rows:
            raise ValueError("capture bytes do not match row pitch and row count")
        tight = b"".join(raw[y*pitch:y*pitch+row_bytes] for y in range(rows))
        levels.append((level["slice"], level["mip"], tight))
    actual = texsub.digest(*values, levels)
    if actual != record.get("content_sha256"):
        raise ValueError("runtime capture content digest mismatch")
    source = {k: record[k] for k in FIELDS}
    source.update(content_sha256=actual, identity_domain="texture-subresources-v1",
                  complete_observed_range=True)
    return source, levels[0][2]


def decode(source, raw):
    w, h = (max(1, source[k] >> source["first_mip"]) for k in ("width", "height"))
    fmt = source["view_format"]
    if fmt in (28, 29, 87, 88, 91, 93):
        pixels = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 4).copy()
        if fmt in (87, 88, 91, 93):
            pixels = pixels[..., [2, 1, 0, 3]]
        if fmt in (88, 93):
            pixels[..., 3] = 255
        return pixels.astype(float)/255
    if fmt == 61:  # used when the shared serializer adds R8_UNORM support
        return np.asarray(Image.frombytes("L", (w, h), raw).convert("RGBA"), dtype=float)/255
    if fmt not in (71, 72, 74, 75, 77, 78, 80, 83, 98, 99):
        raise ValueError("source identity supported, but this decoder/packing is not qualified")
    image = Image.open(io.BytesIO(dds.header(w, h, 1, fmt, len(raw))+raw))
    return np.asarray(image.convert("RGBA"), dtype=float)/255


def read_material(folder, metadata):
    declared = metadata["runtime_maps"]
    if set(declared) != {"base_color", "normal", "roughness"}:
        raise ValueError("coordinated preparation requires three explicitly associated map roles")
    sources, pixels, interpretations = {}, {}, {}
    for role in ("base_color", "normal", "roughness"):
        item = declared[role]
        if "index" in item:
            source, raw = load_dump_entry(child(folder, item["index"]), item["content_sha256"])
        else:
            source, raw = load_capture(child(folder, item["capture"]))
        a, fmt, packing = decode(source, raw), source["view_format"], item["packing"]
        if role == "base_color":
            if fmt not in COLOR or packing != ("rgba_srgb" if fmt in SRGB else "rgba_linear"):
                raise ValueError("colour packing must match the typed view's sampling interpretation")
            if fmt in SRGB:
                a[..., :3] = linear(a[..., :3])
        elif role == "normal":
            if packing == "xy_unorm_positive_z" and fmt in (28, 83):
                xy = a[..., :2]*2-1
                a = unit(np.dstack((xy, np.sqrt(np.maximum(1e-6, 1-np.sum(xy*xy, axis=-1))))))
            elif packing == "xyz_unorm_positive_z" and fmt == 28:
                a = unit(a[..., :3]*2-1)
            else:
                raise ValueError("unqualified tangent normal packing/view")
        else:
            if packing not in ("perceptual_r", "perceptual_g", "perceptual_b", "perceptual_a") or fmt not in (28, 61, 80):
                raise ValueError("unqualified perceptual roughness packing/view")
            channel = "rgba".index(packing[-1])
            if fmt in (61, 80) and channel != 0:
                raise ValueError("single-channel roughness requires the red channel")
            a = a[..., channel]
        sources[role], pixels[role], interpretations[role] = source, a, packing
    return Material(pixels["base_color"], pixels["normal"], pixels["roughness"]), sources, interpretations
