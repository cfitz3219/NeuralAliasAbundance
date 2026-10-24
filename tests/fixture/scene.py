"""A3 deterministic, offline pinhole scene; Python standard library only."""
import argparse
import json
import math
from pathlib import Path

SCHEMA = "neuralalias.fixture.v2"


def camera(frame, width, height):
    cut = frame == 6
    x = 0.12 * frame + (1.8 if frame >= 6 else 0.0)
    y = 0.03 * frame
    focal = height * 0.9
    near, far = 0.1, 30.0
    jitter = ((frame % 4) * 0.25 - 0.375, (frame % 3) / 3.0 - 1.0 / 3.0)
    return dict(position=[x, y, 0.0], focal=[focal, focal],
                principal=[width / 2.0, height / 2.0], jitter=list(jitter),
                near=near, far=far, cut=cut,
                view=[1, 0, 0, -x, 0, 1, 0, -y, 0, 0, 1, 0, 0, 0, 0, 1],
                projection=[2*focal/width, 0, 0, 0, 0, 2*focal/height, 0, 0,
                            0, 0, far/(far-near), -near*far/(far-near), 0, 0, 1, 0])


def objects(frame):
    # id, depth, center x/y, half width/height, animated shear, alpha.
    # Local material coordinates persist across frames, including deformation.
    return [
        (1, 12.0, 0.0, 0.0, 1000.0, 1000.0, 0.0, 1.0),
        (2, 5.0, -1.0 + 0.18*frame, 0.1, 0.9, 0.85, 0.0, 1.0),
        (3, 3.5, 0.8, -0.2, 0.55, 0.8, 0.45*math.sin(frame*0.6), 1.0),
        (4, 2.5, -0.1 + 0.04*frame, 0.3, 0.6, 0.65, 0.0, 0.35),
    ]


def project(cam, world):
    x, y, z = world
    return (cam['principal'][0] + cam['focal'][0]*(x-cam['position'][0])/z,
            cam['principal'][1] - cam['focal'][1]*(y-cam['position'][1])/z)


def unproject(cam, p, z):
    return (cam['position'][0] + (p[0]-cam['principal'][0])*z/cam['focal'][0],
            cam['position'][1] - (p[1]-cam['principal'][1])*z/cam['focal'][1], z)


def hits(cam, geometry, p):
    found = []
    for obj in geometry:
        ident, z, cx, cy, hw, hh, shear, alpha = obj
        wx, wy, _ = unproject(cam, p, z)
        ly = wy-cy
        lx = wx-cx-shear*ly
        if abs(lx) <= hw and abs(ly) <= hh:
            found.append((obj, (lx, ly)))
    return sorted(found, key=lambda hit: hit[0][1])


def color(ident, local):
    # Material-anchored texture provides visible camera/object/deformation motion.
    checker = (math.floor(local[0]*5) + math.floor(local[1]*5)) % 2
    palette = {1: (0.12, 0.23, 0.36), 2: (0.7, 0.12, 0.05),
               3: (0.1, 0.6, 0.15), 4: (0.1, 0.3, 0.8)}
    return [v + checker*0.12 for v in palette[ident]]


def motion_for(hit, previous_geometry, previous_camera, p):
    obj, (lx, ly) = hit
    prior = previous_geometry[obj[0]-1]
    world = (prior[2]+lx+prior[6]*ly, prior[3]+ly, prior[1])
    previous_p = project(previous_camera, world)
    return [previous_p[0]-p[0], previous_p[1]-p[1]], previous_p


def render(frame, width=96, height=64):
    if not (0 <= frame < 120 and 16 <= width <= 1024 and 16 <= height <= 1024):
        raise ValueError("frame must be 0..119; dimensions must be 16..1024")
    cam = camera(frame, width, height)
    prev = camera(max(0, frame-1), width, height)
    geometry = objects(frame)
    previous_geometry = objects(max(0, frame-1))
    reset = frame == 0 or cam['cut']
    planes = {name: [] for name in ('scene_color', 'composite_color', 'depth', 'motion',
              'surface_id', 'history_valid', 'disocclusion', 'hud', 'transparent_alpha',
              'transparent_motion', 'transparent_depth', 'transparent_history_valid')}
    for y in range(height):
        for x in range(width):
            p = (x+0.5-cam['jitter'][0], y+0.5-cam['jitter'][1])
            layers = hits(cam, geometry, p)
            opaque = next(hit for hit in layers if hit[0][7] == 1.0)
            ident, z = opaque[0][:2]
            mv, old_p = motion_for(opaque, previous_geometry, prev, p)
            # Continuous previous-frame visibility at the corresponding material
            # point; independent of raster sampling/nearest-pixel rounding.
            on_screen = 0 <= old_p[0]+prev['jitter'][0] < width and 0 <= old_p[1]+prev['jitter'][1] < height
            old_opaque = next(hit for hit in hits(prev, previous_geometry, old_p) if hit[0][7] == 1.0)
            visible = on_screen and old_opaque[0][0] == ident
            valid = not reset and visible
            base = color(ident, opaque[1])
            glass = next((hit for hit in layers if hit[0][7] < 1.0 and hit[0][1] < z), None)
            alpha, glass_mv, glass_z, glass_valid = 0.0, [0.0, 0.0], 1.0, False
            if glass:
                alpha = glass[0][7]
                tint = color(glass[0][0], glass[1])
                base = [(1-alpha)*v + alpha*t for v, t in zip(base, tint)]
                glass_mv, glass_old_p = motion_for(glass, previous_geometry, prev, p)
                glass_z = glass[0][1]
                # Exact for this scene's single, nearest glass layer. Adding
                # another layer requires checking prior-frame surface identity
                # and occlusion, as the opaque correspondence above does.
                glass_valid = not reset and 0 <= glass_old_p[0]+prev['jitter'][0] < width and 0 <= glass_old_p[1]+prev['jitter'][1] < height
                glass_z = cam['far']/(cam['far']-cam['near']) - cam['near']*cam['far']/((cam['far']-cam['near'])*glass_z)
            hud = x < width//3 and y < height//8
            depth = cam['far']/(cam['far']-cam['near']) - cam['near']*cam['far']/((cam['far']-cam['near'])*z)
            values = dict(scene_color=base, composite_color=[1.0]*3 if hud else base,
                          depth=depth, motion=[0.0, 0.0] if reset else mv, surface_id=ident,
                          history_valid=valid, disocclusion=not reset and not visible,
                          hud=hud, transparent_alpha=alpha,
                          transparent_motion=[0.0, 0.0] if reset else glass_mv,
                          transparent_depth=glass_z, transparent_history_valid=glass_valid)
            for key, value in values.items():
                planes[key].append(value)
    return dict(schema=SCHEMA, frame=frame, width=width, height=height, reset=reset,
                camera=cam, previous_camera=prev,
                conventions=dict(matrix="row-major storage, column vectors, left-handed +Z",
                                 depth="D3D forward Z [0,1], nearest opaque surface",
                                 motion="current-to-previous render pixels, +Y down, excludes jitter",
                                 color="linear SDR RGB; alpha blend before HUD",
                                 visibility="continuous material correspondence; cuts invalidate history"),
                planes=planes)


def write_frame(root, data):
    root.mkdir(parents=True, exist_ok=True)
    prefix = root / ("frame-%03d" % data['frame'])
    prefix.with_suffix('.json').write_text(json.dumps(data, allow_nan=False), encoding='utf-8')
    for key in ('scene_color', 'composite_color'):
        rgb = bytes(round(max(0, min(1, v))*255) for pixel in data['planes'][key] for v in pixel)
        (root / (prefix.name+'-'+key+'.ppm')).write_bytes(
            ("P6\n%d %d\n255\n" % (data['width'], data['height'])).encode('ascii') + rgb)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--width', type=int, default=96)
    parser.add_argument('--height', type=int, default=64)
    parser.add_argument('--frames', type=int, default=12)
    args = parser.parse_args()
    if not 1 <= args.frames <= 120:
        parser.error('--frames must be 1..120')
    for frame in range(args.frames):
        write_frame(args.output, render(frame, args.width, args.height))
    print('Wrote %d frames and truth files to %s' % (args.frames, args.output))


if __name__ == '__main__':
    main()
