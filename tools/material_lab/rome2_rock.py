"""Prepare native-size normal maps for the Rome II campaign rock_b family."""

import argparse
from pathlib import Path

import numpy as np

from . import rome2
from .detail import down2, unit
from .learned_surface import slopes


ROCK_RELIEF_STRENGTH = .65


def bake_normal(source_path, destination, texconv):
    source, fmt, levels = rome2.read_dds(source_path)
    if fmt != rome2.LEGACY[b'DXT5'][0]:
        raise ValueError(f'Expected alpha/green DXT5 normals: {source_path}')
    authored = rome2.normals(rome2.pixels(levels[0], fmt))
    delta = ROCK_RELIEF_STRENGTH * rome2.terrain_relief_slopes(authored, authored)
    result = []
    for level in levels:
        width, height, _ = level
        authored = rome2.normals(rome2.pixels(level, fmt))
        xy = slopes(authored) + delta
        normal = unit(np.dstack((xy, np.ones((height, width), np.float32))))
        result.append(rome2.normal_bytes(normal))
        delta = down2(delta)
    rome2.encode(result, source, destination, texconv)
    # Preserve the game's header and any padding beyond its compressed chain.
    end = 128 + sum(len(blocks) for _, _, blocks in levels)
    data = destination.read_bytes()
    destination.write_bytes(source[:128] + data[128:end] + source[end:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source_dir', type=Path)
    parser.add_argument('output_dir', type=Path)
    parser.add_argument('texconv', type=Path)
    args = parser.parse_args()
    for variant in range(4):
        name = f'{variant}_su_normal.dds'
        bake_normal(args.source_dir / name, args.output_dir / name, args.texconv.resolve())


if __name__ == '__main__':
    main()
