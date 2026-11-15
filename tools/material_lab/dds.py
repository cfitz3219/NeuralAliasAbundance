"""DDS+DX10 layout and per-mip block compression using Microsoft's texconv.

Compress each already-filtered mip individually. Never let a colour-image mip
generator replace our normal/roughness moment filtering.
"""
import struct
import subprocess
from pathlib import Path
import tempfile
import numpy as np
from PIL import Image
from .detail import srgb

FORMATS = {"base_color": (99, "BC7_UNORM_SRGB", 16),
           "normal": (83, "BC5_UNORM", 16), "roughness": (80, "BC4_UNORM", 8)}


def header(width, height, count, fmt, first_size):
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000
    caps = 0x1000
    if count > 1:
        flags |= 0x20000
        caps |= 0x8 | 0x400000
    words = [124, flags, height, width, first_size, 0, count] + [0]*11
    words += [32, 4, int.from_bytes(b"DX10", "little"), 0, 0, 0, 0, 0]
    words += [caps, 0, 0, 0, 0]
    return b"DDS " + struct.pack("<31I", *words) + struct.pack("<5I", fmt, 3, 0, 1, 1 if fmt == 99 else 3)


def layout(data):
    if len(data) < 148 or data[:4] != b"DDS " or data[84:88] != b"DX10":
        raise ValueError("expected DDS with DX10 header")
    words = struct.unpack_from("<31I", data, 4)
    fmt, dim, misc, arrays, _ = struct.unpack_from("<5I", data, 128)
    if words[0] != 124 or dim != 3 or misc or arrays != 1 or fmt not in (99, 83, 84, 80):
        raise ValueError("unsupported prepared DDS layout")
    w, h, count = words[3], words[2], words[6]
    if not w or not h or not 1 <= count <= max(w, h).bit_length() or max(w, h) > 16384:
        raise ValueError("invalid DDS dimensions/mips")
    block = 8 if fmt == 80 else 16
    mips, offset = [], 148
    for level in range(count):
        pitch = ((w+3)//4)*block
        size = pitch*((h+3)//4)
        mips.append(dict(level=level, width=w, height=h, offset=offset, row_pitch=pitch, byte_size=size))
        offset += size
        w, h = max(1, w//2), max(1, h//2)
    if offset != len(data):
        raise ValueError("DDS payload does not match mip layout")
    return fmt, mips


def pixels(material, role):
    if role == "base_color":
        a = np.concatenate([srgb(material.color[..., :3]), material.color[..., 3:4]], -1)
    elif role == "normal":
        a = material.normal*.5+.5
    else:
        a = material.roughness
    return Image.fromarray(np.round(np.clip(a, 0, 1)*255).astype(np.uint8))


def decode_signed_bc5(raw, width, height, texconv):
    """Use DirectXTex's signed decoder, retaining negative components as floats.

    Pillow treats BC5 SNORM like UNORM in some releases. Reinterpreting its
    bytes or biasing that incorrect decode cannot recover the signed endpoints.
    This is an offline conversion, never a live-game operation.
    """
    with tempfile.TemporaryDirectory(prefix="na-signed-normal-") as temp:
        root = Path(temp)
        src = root/"signed.dds"
        src.write_bytes(header(width, height, 1, 84, len(raw))+raw)
        dest = root/"decoded"
        dest.mkdir()
        run = subprocess.run([str(texconv), "-nologo", "-nogpu", "--single-proc",
                              "-dx10", "-m", "1", "-f", "R32G32_FLOAT", "-o",
                              str(dest), "-y", str(src)],
                             capture_output=True, text=True, timeout=120)
        if run.returncode:
            raise RuntimeError(f"signed normal decode: {run.stdout} {run.stderr}")
        data = (dest/"signed.dds").read_bytes()
        if (len(data) != 148+width*height*8 or data[84:88] != b"DX10" or
                struct.unpack_from("<I", data, 128)[0] != 16):
            raise ValueError("signed decoder returned an unexpected float layout")
        xy = np.frombuffer(data, dtype="<f4", offset=148).reshape(height, width, 2).copy()
        if not np.isfinite(xy).all() or np.max(np.abs(xy)) > 1.00001:
            raise ValueError("invalid signed normal components")
        return xy


def compress(chain, role, path, texconv, *, signed_normal=False):
    fmt, name, _ = FORMATS[role]
    if signed_normal:
        if role != "normal":
            raise ValueError("signed encoding is only supported for normal maps")
        fmt, name = 84, "BC5_SNORM"
    blocks = []
    with tempfile.TemporaryDirectory(prefix="na-material-mips-") as temp:
        root = Path(temp)
        for level, material in enumerate(chain):
            png = root/f"mip{level}.png"
            if signed_normal:
                # Feed signed floats directly: no UNORM conversion, extra bias,
                # quantization through an 8-bit PNG, or silent negative clamp.
                png = root/f"mip{level}.dds"
                xy = np.asarray(material.normal[..., :2], dtype="<f4")
                h, w = xy.shape[:2]
                png.write_bytes(header(w, h, 1, 16, w*h*8)+xy.tobytes())
            else:
                pixels(material, role).save(png)
            out = root/f"compressed{level}"
            out.mkdir()
            args = [str(texconv), "-nologo", "-nogpu", "--single-proc", "-dx10", "-m", "1",
                    "-f", name, "-o", str(out), "-y"]
            if role == "base_color":
                args += ["-srgb", "-bc", "q"]
            else:
                args += ["--ignore-srgb"]
            run = subprocess.run(args+[str(png)], capture_output=True, text=True, timeout=120)
            if run.returncode:
                raise RuntimeError(f"texconv {role} mip {level}: {run.stdout} {run.stderr}")
            payload = (out/png.with_suffix(".dds").name).read_bytes()
            actual, levels = layout(payload)
            if actual != fmt or len(levels) != 1:
                raise ValueError("compressor changed the requested format/mip count")
            blocks.append(payload[148:])
    h, w = chain[0].roughness.shape
    data = header(w, h, len(chain), fmt, len(blocks[0])) + b"".join(blocks)
    layout(data)
    Path(path).write_bytes(data)
    return data
