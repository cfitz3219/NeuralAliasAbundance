"""Production depth evidence against fixture-v2 geometry; no oracle mask input."""
import argparse
from collections import Counter
import subprocess
from scene import render
from test_signal_content import matrix_product


def evaluate(runner, previous, current, corrupt=False):
    width, height = current['width'], current['height']
    parts = [f"{width} {height} {int(current['camera']['cut'])}"]
    for n, frame in enumerate((previous, current)):
        cam = frame['camera']
        parts.append(' '.join(map(str, matrix_product(cam['projection'], cam['view']) + cam['jitter'])))
        values = frame['planes']['depth']
        if corrupt and n:
            values = [d - 0.05 for d in values]
        parts.append(' '.join(map(str, values)))
    result = subprocess.run([runner], input='\n'.join(parts), text=True, capture_output=True, timeout=10)
    if result.returncode:
        raise AssertionError(result.stderr or f'runner exit {result.returncode}')
    return int(result.stdout.split()[0])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', required=True)
    args = parser.parse_args()
    counts = Counter()
    for width, height in ((64, 48), (96, 64), (160, 96)):
        for number in range(1, 12):
            previous, current = render(number-1, width, height), render(number, width, height)
            for corrupt in (False, True):
                verdict = evaluate(args.runner, previous, current, corrupt)
                counts[(corrupt, verdict)] += 1
                if current['camera']['cut'] and verdict != 0:
                    raise AssertionError('camera cut did not invalidate temporal evidence')
                if corrupt and verdict == 3:
                    raise AssertionError('inconsistent depth versions were corroborated')
    if not counts[(False, 3)] or not counts[(True, 2)]:
        raise AssertionError(f'no informative positive/negative coverage: {counts}')
    print('66 temporal depth pairs, depth/camera inputs only:', sorted(counts.items()))
    print('Evidence agreement is not semantic depth identification; A5 acceptance is separate.')


if __name__ == '__main__':
    main()
