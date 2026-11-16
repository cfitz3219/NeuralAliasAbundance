"""Bake Rome II colour and DXT5nm relief, and preview the exact packed mip data.

Inputs are selected, extracted DDS paths. Use RPFM for archive reading and
writing. Only opaque power-of-two colour and alpha/green positive-Z normals
are supported here. All other material maps stay in the original archives.
"""
import argparse
import base64
import io
import json
from pathlib import Path, PurePosixPath
import struct
import subprocess
import tempfile

import numpy as np
from PIL import Image

from . import cache, dds, viewer
from .detail import Material, down2, linear, mip_chain, srgb, unit
from .learned_surface import combine, lowpass, preserve_authored_direction, resize_wrap, slopes, SETTINGS
from .runtime_prepare import predict_pair


LEGACY = {b'DXT1': (71, 8, 'BC1_UNORM'), b'DXT5': (77, 16, 'BC3_UNORM')}
TERRAIN_RELIEF = dict(near_sigma=5., far_sigma=32., gain=14., max_slope=.55)


def terrain_relief_slopes(authored_normal, prediction):
    """Extract and bound broader relief in the original texture coordinates."""
    if authored_normal.shape != prediction.shape:
        raise ValueError('Terrain prediction must match the authored texture size')
    opts = TERRAIN_RELIEF
    predicted = slopes(prediction)
    broad = lowpass(predicted, opts['near_sigma']) - lowpass(predicted, opts['far_sigma'])
    authored = lowpass(slopes(authored_normal), opts['near_sigma'])
    broad = preserve_authored_direction(broad, authored) * opts['gain']
    magnitude = np.linalg.norm(broad, axis=-1, keepdims=True)
    return broad * np.minimum(1., opts['max_slope']/np.maximum(magnitude, 1e-12))


def add_terrain_relief(material, authored_normal, prediction):
    broad = terrain_relief_slopes(authored_normal, prediction)
    h, w = material.normal.shape[:2]
    xy = slopes(material.normal) + resize_wrap(broad, h, w)
    return Material(material.color, unit(np.dstack((xy, np.ones((h,w))))), material.roughness)


def read_dds(path, *, allow_npot=False):
    data = path.read_bytes()
    if data[:4] != b'DDS ' or data[84:88] not in LEGACY:
        raise ValueError(f'Unsupported legacy DDS: {path}')
    fmt, block, _ = LEGACY[data[84:88]]
    height, width = struct.unpack_from('<II', data, 12)
    count = struct.unpack_from('<I', data, 28)[0]
    if min(width, height) < 1 or (not allow_npot and (width & (width - 1) or height & (height - 1))):
        raise ValueError(f'Unsupported DDS dimensions: {path}')
    if count != max(width, height).bit_length():
        raise ValueError(f'Expected a complete DDS chain: {path}')
    levels, at = [], 128
    for _ in range(count):
        size = max(1, (width + 3)//4)*max(1, (height + 3)//4)*block
        levels.append((width, height, data[at:at+size]))
        at += size
        width, height = max(1, width//2), max(1, height//2)
    # CA's writer can append zero padding after the last compressed block.
    if at > len(data) or any(data[at:]):
        raise ValueError(f'Unexpected DDS payload: {path}')
    return data, fmt, levels


def pixels(level, fmt):
    width, height, blocks = level
    raw = dds.header(width, height, 1, fmt, len(blocks))+blocks
    return np.asarray(Image.open(io.BytesIO(raw)).convert('RGBA')).copy()


def normals(rgba):
    if np.any(rgba[..., 0] != 255) or np.any(rgba[..., 2] != 0):
        raise ValueError('Normal is not the selected Rome II alpha/green layout')
    xy = rgba[..., [3, 1]].astype(np.float32)/127.5-1
    return unit(np.dstack((xy, np.sqrt(np.maximum(1e-8, 1-np.sum(xy*xy, axis=-1))))))


def encode(levels, source_header, path, texconv, *, allow_npot=False):
    """Compress explicit colour/normal mips, preserving the legacy DDS layout."""
    fourcc = source_header[84:88]
    blocks = []
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        output = work/'out'
        output.mkdir()
        for number, arr in enumerate(levels):
            png = work/f'mip{number}.png'
            Image.fromarray(arr).save(png)
            subprocess.run([str(texconv), '-nologo', '-nogpu', '--single-proc', '-m', '1',
                            '-f', LEGACY[fourcc][2], '-o', str(output), '-y', str(png)],
                           check=True, capture_output=True, timeout=120)
            encoded = (output/png.with_suffix('.dds').name).read_bytes()
            if encoded[84:88] != fourcc:
                raise ValueError('Compressor changed legacy DDS format')
            blocks.append(encoded[128:])
    h, w = levels[0].shape[:2]
    header = bytearray(source_header[:128])
    struct.pack_into('<III', header, 12, h, w, len(blocks[0]))
    struct.pack_into('<I', header, 28, len(blocks))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header+b''.join(blocks))
    read_dds(path, allow_npot=allow_npot)


def keep_native_mips(prepared, original, output):
    """Deliver the baked levels that fit the authored terrain texture layout."""
    _, prepared_format, prepared_mips = read_dds(prepared)
    source, source_format, source_mips = read_dds(original)
    if prepared_format != source_format or [mip[:2] for mip in prepared_mips[1:]] != [mip[:2] for mip in source_mips]:
        raise ValueError('Prepared DDS is not a 2x bake of the authored layout')
    payload = b''.join(blocks for _, _, blocks in prepared_mips[1:])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(source[:128]+payload+source[128+len(payload):])


def colour_bytes(material):
    return np.rint(srgb(material.color[..., :3])*255).clip(0,255).astype(np.uint8)


def normal_bytes(normal):
    xy = np.rint((normal[..., :2]*.5+.5)*255).clip(0,255).astype(np.uint8)
    r = np.full(xy.shape[:2], 255, np.uint8)
    return np.stack((r, xy[..., 1], np.zeros_like(r), xy[..., 0]), axis=-1)


def png_url(array):
    buf = io.BytesIO()
    Image.fromarray(array).save(buf, format='PNG')
    return 'data:image/png;base64,'+base64.b64encode(buf.getvalue()).decode('ascii')


def preview(colour, normal, label, gloss):
    color_data, color_fmt, color_mips = read_dds(colour)
    norm_data, norm_fmt, norm_mips = read_dds(normal)
    return dict(label=label, bytes=len(color_data)+len(norm_data)-256,
                dimensions={'base_color': color_mips[0][:2]},
                maps={'base_color': [png_url(pixels(m, color_fmt)) for m in color_mips],
                      'normal': [png_url(pixels(m, norm_fmt)[..., [3, 1, 2]]) for m in norm_mips],
                      'roughness': gloss})


def prepare(row, input_root, output, texconv, colour_model, normal_model):
    paths = [PurePosixPath(row['paths'][role]) for role in ('base_color','normal')]
    if any(p.is_absolute() or '..' in p.parts or p.suffix != '.dds' for p in paths):
        raise ValueError('Expected relative DDS asset paths')
    sources = [input_root/source/path for source,path in zip(row['extracts'],paths)]
    (c, cf, cm), (n, nf, nm) = (read_dds(p) for p in sources)
    source_color, source_normal = pixels(cm[0], cf), pixels(nm[0], nf)
    if np.any(source_color[..., 3] != 255) or nf != 77 or source_color.shape != source_normal.shape:
        raise ValueError('Expected matching opaque colour and DXT5 normal textures')
    normal = normals(source_normal)
    placeholder = np.full(normal.shape[:2], .65, np.float32)
    rgba = np.dstack((linear(source_color[..., :3].astype(np.float32)/255), np.ones_like(placeholder)))
    original = Material(rgba, normal, placeholder)
    identity = {'base_color': dict(path=str(paths[0]), sha256=cache.digest(c))}
    colour, fine, medium, inference = predict_pair(original, identity, colour_model, normal_model, output/'inference')
    h,w = colour.shape[:2]
    model_colour = np.dstack((linear(colour.astype(np.float32)/255), np.ones((h,w), np.float32)))
    enhanced = combine(original, model_colour, fine, np.full(fine.shape[:2], .65, np.float32), .65,
                       medium_normal=medium, medium_roughness=np.full(medium.shape[:2], .65, np.float32),
                       fine_downsample=SETTINGS['fine_downsample'])
    # Preserve the existing v4 detail scale in UV space, then ship the next mip
    # as the top level. This caps the asset at 2x dimensions rather than 4x.
    chain = mip_chain(enhanced)[1:]
    broad_relief = bool(row.get('broad_relief'))
    if broad_relief:
        # Bake at the shipping resolution, then filter the widened relief into
        # every mip. Chain[0] retains the old result as the close-detail base.
        chain = mip_chain(add_terrain_relief(chain[0], normal, medium))
    targets = [output/'overrides'/p for p in paths]
    encode([colour_bytes(m) for m in chain], c, targets[0], texconv)
    encode([normal_bytes(m.normal) for m in chain], n, targets[1], texconv)
    if row.get('native_extent'):
        for target, source in zip(targets, sources, strict=True):
            keep_native_mips(target, source, target)
    gloss = [png_url(np.full((1,1,3), 166, dtype=np.uint8))]
    before = preview(*sources, 'Original archive textures', gloss)
    after = preview(*targets, 'Prepared archive textures', gloss)
    baseline = dict(before, label='PBRify alone', description='Enhanced colour from the same output file.',
                    bytes=targets[0].stat().st_size+len(n)-256, dimensions=after['dimensions'],
                    maps={**before['maps'], 'base_color':after['maps']['base_color']})
    del after['maps']['base_color']
    retained = dict(material=row['name'], paths=list(map(str, paths)),
                    source_sha256=list(map(cache.digest, (c,n))), output_sha256=[cache.digest(p.read_bytes()) for p in targets],
                    source_bytes=before['bytes'], output_bytes=after['bytes'], inference=inference,
                    terrain_relief=TERRAIN_RELIEF if broad_relief else None)
    display = dict(name=row['name'], family=row['family'], game_asset=True, combined=True,
                   original=before, comparisons={'baseline':baseline}, coordinated=after,
                   broad_relief=broad_relief)
    return display, retained


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('selection','input','output','texconv','colour-model','normal-model'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    rows, receipts = [], []
    args.output.mkdir(parents=True, exist_ok=True)
    for row in json.loads(args.selection.read_text(encoding='utf-8-sig')):
        print(f'Preparing {row["name"]}', flush=True)
        display, receipt = prepare(row, args.input, args.output, args.texconv.resolve(), args.colour_model, args.normal_model)
        rows.append(display)
        receipts.append(receipt)
        print(f'{receipt["output_bytes"]:,} compressed bytes', flush=True)
    viewer.write_payload(rows, args.output/'Rome-II-Materials.html')
    (args.output/'receipt.json').write_text(json.dumps(receipts, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
