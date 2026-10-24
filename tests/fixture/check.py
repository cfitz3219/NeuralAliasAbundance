"""Compare a candidate's exported planes against A3 reference truth."""
import argparse
import json
import math
from pathlib import Path
from scene import SCHEMA

COMPONENTS = dict(scene_color=3, depth=1, motion=2, history_valid=0,
                  transparent_motion=2, transparent_depth=1, transparent_history_valid=0)


def validate(data):
    if not isinstance(data, dict) or data.get('schema') != SCHEMA:
        raise ValueError('unsupported schema')
    for key in ('width', 'height', 'frame'):
        if type(data.get(key)) is not int:
            raise ValueError('invalid '+key)
    if not (16 <= data['width'] <= 1024 and 16 <= data['height'] <= 1024 and 0 <= data['frame'] < 120):
        raise ValueError('invalid frame/dimensions')
    if type(data.get('reset')) is not bool or not isinstance(data.get('planes'), dict):
        raise ValueError('invalid reset or planes')
    for camera_name in ('camera', 'previous_camera'):
        cam = data.get(camera_name, {})
        if not isinstance(cam, dict):
            raise ValueError('invalid '+camera_name)
        for name, count in dict(position=3, focal=2, principal=2, jitter=2, view=16, projection=16).items():
            values = cam.get(name)
            if not isinstance(values, list) or len(values) != count or any(
                    type(v) not in (int, float) or not math.isfinite(v) for v in values):
                raise ValueError('missing/invalid camera '+name)
        for name in ('near', 'far'):
            if type(cam.get(name)) not in (int, float) or not math.isfinite(cam[name]):
                raise ValueError('missing/invalid camera '+name)
        if not 0 < cam['near'] < cam['far']:
            raise ValueError('invalid near/far range')
    n = data['width']*data['height']
    for name, components in COMPONENTS.items():
        values = data.get('planes', {}).get(name)
        if not isinstance(values, list) or len(values) != n:
            raise ValueError('missing/truncated plane '+name)
        for value in values:
            if components == 0:
                if type(value) is not bool:
                    raise ValueError('non-boolean validity '+name)
                continue
            vector = value if components > 1 else [value]
            if not isinstance(vector, list) or len(vector) != components:
                raise ValueError('wrong component count '+name)
            if any(type(v) not in (int, float) or not math.isfinite(v) for v in vector):
                raise ValueError('non-finite/non-numeric value '+name)


def compare(truth, candidate, tolerance=1e-4):
    if not math.isfinite(tolerance) or tolerance < 0:
        raise ValueError('tolerance must be finite and nonnegative')
    validate(truth)
    validate(candidate)
    for key in ('frame', 'width', 'height', 'reset', 'conventions'):
        if key not in truth or candidate.get(key) != truth[key]:
            raise ValueError('mismatched '+key)
    n = truth['width']*truth['height']
    result = {}
    for name, components in COMPONENTS.items():
        errors, maximum, checked = 0, 0.0, 0
        validity = {'motion': 'history_valid', 'transparent_motion': 'transparent_history_valid'}.get(name)
        for i in range(n):
            if validity and not truth['planes'][validity][i]:
                continue
            a, b = truth['planes'][name][i], candidate['planes'][name][i]
            if components == 0:
                error = float(a != b)
            elif components == 1:
                error = abs(a-b)
            else:
                error = max(abs(x-y) for x, y in zip(a, b))
            checked += 1
            maximum = max(maximum, error)
            errors += error > (0.0 if components == 0 else tolerance)
        result[name] = dict(checked=checked, errors=errors, max_error=maximum)
    camera_errors = 0
    for camera_name in ('camera', 'previous_camera'):
        for name in ('position', 'focal', 'principal', 'jitter', 'view', 'projection', 'near', 'far'):
            a, b = truth[camera_name][name], candidate[camera_name][name]
            if not isinstance(a, list): a, b = [a], [b]
            camera_errors += sum(abs(x-y) > tolerance for x, y in zip(a, b))
    return dict(passed=camera_errors == 0 and all(r['errors'] == 0 for r in result.values()),
                camera_errors=camera_errors, planes=result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--truth', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--tolerance', type=float, default=1e-4)
    args = parser.parse_args()
    try:
        result = compare(json.loads(args.truth.read_text(encoding='utf-8')),
                         json.loads(args.candidate.read_text(encoding='utf-8')), args.tolerance)
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(json.dumps(dict(passed=False, error=str(error))))
        return 2
    print(json.dumps(result, allow_nan=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
