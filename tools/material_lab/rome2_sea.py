"""Regrade Rome II's campaign sea palette with the original BC3 alpha intact."""
import argparse
from pathlib import Path
import tempfile

import numpy as np
from PIL import Image, ImageFilter

from .rome2 import encode, pixels, read_dds


def smoothstep(lo, hi, x):
    t = np.clip((x-lo)/(hi-lo), 0., 1.)
    return t*t*(3.-2.*t)


def shore_field(alpha):
    # Vanilla Grand Campaign and Imperator Augustus encode a land/water
    # boundary in these maps. Use it to locate the coast, preserve its bytes.
    proximity = np.rint(smoothstep(5, 28, alpha.astype(np.float32))*255).astype(np.uint8)
    return Image.fromarray(proximity).filter(ImageFilter.GaussianBlur(radius=12))


def grade(rgba, smoothed_shore):
    rgb = rgba[..., :3].astype(np.float32)
    water = 1.-smoothstep(12, 28, rgba[..., 3].astype(np.float32))
    coast = smoothstep(8, 115, smoothed_shore.astype(np.float32))[..., None]
    deep = rgb*[.58, .82, 1.10]+[1, 3, 6]
    shallow = rgb*[.55, 1.22, 1.10]+[4, 14, 10]
    palette = deep+(shallow-deep)*coast
    return np.rint(np.clip(rgb+(palette-rgb)*water[..., None],0,255)).astype(np.uint8)


def merge_blocks(original, compressed, rgb_changed):
    width, height, original_bytes = original
    if compressed[:2] != (width,height):
        raise ValueError('Colour compressor changed the source mip dimensions')
    rows, cols = (height+3)//4, (width+3)//4
    changed = np.pad(rgb_changed, ((0,rows*4-height),(0,cols*4-width)))
    changed = changed.reshape(rows,4,cols,4).any(axis=(1,3)).ravel()
    merged = np.frombuffer(original_bytes,np.uint8).reshape(-1,16).copy()
    new = np.frombuffer(compressed[2],np.uint8).reshape(-1,16)
    # BC3 consists of an 8-byte alpha block followed by an independent colour
    # block. Untouched areas retain their authored colour blocks too.
    merged[changed,8:] = new[changed,8:]
    return merged.tobytes()


def prepare(source, target, texconv):
    data, fmt, mips = read_dds(source, allow_npot=True)
    if fmt != 77 or mips[0][:2] != (2032,1440):
        raise ValueError('Expected the 2032×1440 BC3 campaign sea map')
    images = [pixels(mip,fmt) for mip in mips]
    shore = shore_field(images[0][...,3])
    colours = [grade(img, np.asarray(shore.resize(mip[:2], Image.Resampling.BOX)))
               for img,mip in zip(images,mips)]
    with tempfile.TemporaryDirectory() as temp:
        rgb_dds = Path(temp)/'colour.dds'
        encode(colours,data,rgb_dds,texconv,allow_npot=True)
        _, _, encoded = read_dds(rgb_dds,allow_npot=True)
    chunks = [merge_blocks(mip,new,(img[...,:3]!=rgb).any(axis=-1))
              for mip,new,img,rgb in zip(mips,encoded,images,colours,strict=True)]
    # Keep even the original header and any padding after the last mip.
    end = 128+sum(map(len,chunks))
    target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes(data[:128]+b''.join(chunks)+data[end:])
    output, _, _ = read_dds(target,allow_npot=True)
    if len(output)!=len(data):
        raise ValueError('Campaign DDS storage size changed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source','output','texconv'):
        parser.add_argument(name,type=Path)
    args = parser.parse_args()
    prepare(args.source, args.output, args.texconv.resolve())


if __name__ == '__main__':
    main()
