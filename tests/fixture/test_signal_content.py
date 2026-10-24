"""Exercise production C++ content checks on anonymous fixture-v2 candidates.

Independent world geometry and controlled blend probes are supplied as evidence.
Candidate labels remain here in the scorer, never in the C++ input.
This is offline corroboration, not automatic live acquisition of that evidence.
"""
import argparse
import random
import subprocess
from scene import render, objects, hits, unproject


def matrix_product(a, b):
    return [sum(a[r*4+k]*b[k*4+c] for k in range(4))
            for r in range(4) for c in range(4)]


def case(runner, frame, width, height, opaque_only=False, wrong_camera=False):
    data = render(frame, width, height)
    prior = render(max(0, frame-1), width, height)
    cam, planes = data['camera'], data['planes']
    n = width*height
    # Independent scene intersections, NOT inversion of the candidate depth.
    world = []
    for y in range(height):
        for x in range(width):
            p = (x+0.5-cam['jitter'][0], y+0.5-cam['jitter'][1])
            surface = next(h for h in hits(cam, objects(frame), p) if h[0][7] == 1)
            world.append(unproject(cam, p, surface[0][1]))
    candidates = [
        ('depth', 0, 0, planes['depth'], 2),
        ('reverse_depth', 0, 1, [1-d for d in planes['depth']], 2),
        ('wrong_convention', 0, 0, [1-d for d in planes['depth']], 1),
        ('shadow', 0, 0, [max(0,d-0.02) for d in planes['depth']], 1),
        ('clear', 0, 0, [1.0]*n, 1),
        ('scene', 1, 0, planes['scene_color'], 2),
        ('hud_composite', 1, 0, planes['composite_color'], 1),
        ('postprocess', 1, 0, [[v*0.8 for v in p] for p in planes['scene_color']], 1),
        ('history', 1, 0, prior['planes']['scene_color'], 1 if frame else 2),
    ]
    rng = random.Random(frame*1000+width)
    rng.shuffle(candidates)
    ids = rng.sample(range(100,1000000), len(candidates))
    witnesses = 1 if opaque_only else 2
    parts = [f'{width} {height} {len(candidates)} {witnesses}']
    vp = matrix_product(cam['projection'], cam['view'])
    if wrong_camera:
        vp[3] += 0.05
    parts.append(' '.join(map(str, vp+cam['jitter'])))
    parts.extend(' '.join(map(str,p)) for p in world)
    for probe in range(witnesses):
        for i, scene in enumerate(planes['scene_color']):
            # Probe 0 is actual A3 opaque HUD. Probe 1 is a controlled additional
            # composition of the SAME scene version with translucent UI. It
            # exposes opaque-HUD pixels otherwise fundamentally unidentifiable.
            alpha = (1.0 if probe == 0 else 0.5) if planes['hud'][i] else 0.0
            foreground = [alpha]*3
            composite = planes['composite_color'][i] if probe == 0 else [
                (1-alpha)*v+alpha for v in scene]
            parts.append(' '.join(map(str,[alpha]+foreground+composite)))
    expected = {}
    for ident, (label, kind, reverse, values, verdict) in zip(ids, candidates):
        parts.append(f'{ident} {kind} {reverse}')
        if kind == 0:
            parts.append(' '.join(map(str,values)))
        else:
            parts.extend(' '.join(map(str,p)) for p in values)
        if wrong_camera and kind == 0:
            verdict = 0
        if opaque_only and label in ('scene','hud_composite'):
            verdict = 0  # Neither can be distinguished under a fully opaque HUD.
        if opaque_only and label == 'history' and frame == 0:
            verdict = 0
        expected[ident] = (label,verdict)
    result = subprocess.run([runner], input='\n'.join(parts)+'\n', text=True,
                            capture_output=True, timeout=20)
    if result.returncode:
        raise AssertionError(f'runner failed {result.returncode}: {result.stderr}')
    seen = set()
    for line in result.stdout.splitlines():
        ident, verdict, checked, unresolved, mismatches = map(int,line.split())
        if ident in seen or ident not in expected:
            raise AssertionError('duplicate/unknown output identity')
        seen.add(ident)
        label, wanted = expected[ident]
        if verdict != wanted:
            raise AssertionError(f'frame={frame} {width}x{height} opaque={opaque_only} '
                                 f'camera={wrong_camera} {label}: {verdict} != {wanted}')
        if verdict == 2 and (checked != n or unresolved or mismatches):
            raise AssertionError('consistency reported without full coverage')
    if seen != expected.keys():
        raise AssertionError('missing candidate result')
    return len(candidates)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', required=True)
    args = parser.parse_args()
    decisions = 0
    for width,height in ((32,24),(47,31),(64,48)):
        for frame in range(12):
            decisions += case(args.runner,frame,width,height)
    for frame in (0,3,6,11):
        decisions += case(args.runner,frame,47,31,opaque_only=True)
        decisions += case(args.runner,frame,47,31,wrong_camera=True)
    print(f'{decisions} anonymous candidate decisions matched; zero false Consistent results. '
          'Independent fixture geometry + controlled composition probes supplied; '
          'not live discovery or A5 end-to-end acceptance.')


if __name__ == '__main__':
    main()
