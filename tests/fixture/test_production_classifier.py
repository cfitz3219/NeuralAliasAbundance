"""A5: anonymous candidate decisions through metadata AND production checks.

The scorer retains truth labels. Input contains observed write order/version,
composition source lineage, overlay scissor bounds, camera and completed pixels.
No world positions, HUD foreground/alpha, or oracle visibility mask is sent.
"""
import argparse
import json
from pathlib import Path
import random
import subprocess
from scene import render
from test_signal_content import matrix_product


def case(runner, number, width, height):
    previous, current = render(number-1, width, height), render(number, width, height)
    p, c = previous['planes'], current['planes']
    frame = number+1
    rng = random.Random(width*10000+number)
    ids = rng.sample(range(100, 1000000), 8)
    # Write order/version and the consumed source are fixture render-graph
    # observations. Labels below are used only by this external scorer.
    entries = [
        (ids[0], 'depth', 0, 100, frame, p['depth'], c['depth'], True),
        (ids[1], 'scene', 1, 100, frame, None, c['scene_color'], True),
        (ids[2], 'shadow', 0, 50, frame, [d-.02 for d in p['depth']], [d-.02 for d in c['depth']], False),
        (ids[3], 'clear', 0, 100, frame, [1.0]*len(p['depth']), [1.0]*len(c['depth']), False),
        (ids[4], 'wrong_z', 0, 100, frame, [1-d for d in p['depth']], [1-d for d in c['depth']], False),
        (ids[5], 'hud_final', 1, 121, frame, None, c['composite_color'], False),
        (ids[6], 'history', 1, 100, frame-1, None, p['scene_color'], False),
        (ids[7], 'postprocess', 1, 100, frame, None, [[v*.8 for v in pixel] for pixel in c['scene_color']], False),
    ]
    rng.shuffle(entries)
    parts = [f"{width} {height} {len(entries)} {int(current['reset'])} {frame} {ids[1]}"]
    for image in (previous, current):
        camera = image['camera']
        parts.append(' '.join(map(str, matrix_product(camera['projection'], camera['view']) + camera['jitter'])))
    parts.append(f'0 0 {width//3} {height//8}')  # observed overlay scissor, not per-pixel HUD truth
    parts.extend(' '.join(map(str, pixel)) for pixel in c['composite_color'])
    truth = {}
    for ident, label, kind, write, version, prior, values, positive in entries:
        parts.append(f'{ident} {kind} {write} {version}')
        if kind:
            parts.extend(' '.join(map(str, pixel)) for pixel in values)
        else:
            parts.append(' '.join(map(str, prior)))
            parts.append(' '.join(map(str, values)))
        truth[ident] = (label, positive and not (label == 'depth' and current['reset']))
    result = subprocess.run([runner], input='\n'.join(parts), text=True, capture_output=True, timeout=20)
    if result.returncode:
        raise AssertionError(result.stderr or f'runner exit {result.returncode}')
    seen, records = set(), []
    for line in result.stdout.splitlines():
        ident, verdict, checked, mismatches = map(int, line.split())
        if ident not in truth or ident in seen:
            raise AssertionError('invalid result identity')
        seen.add(ident)
        label, positive = truth[ident]
        records.append(dict(frame=number, width=width, height=height, label=label,
                            expected_supported=positive, supported=verdict == 2, verdict=verdict,
                            checked=checked, mismatches=mismatches))
    if seen != truth.keys():
        raise AssertionError('missing candidate result')
    return records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', required=True)
    parser.add_argument('--report')
    args = parser.parse_args()
    records = [record for size in ((64,48),(96,64),(160,96)) for frame in range(1,12)
               for record in case(args.runner,frame,*size)]
    tp = sum(r['supported'] and r['expected_supported'] for r in records)
    tn = sum(not r['supported'] and not r['expected_supported'] for r in records)
    fp = sum(r['supported'] and not r['expected_supported'] for r in records)
    fn = sum(not r['supported'] and r['expected_supported'] for r in records)
    report = dict(schema='neuralalias.a5.fixture.acceptance.v1', cases=len(records), true_positive=tp,
                  true_negative=tn, false_positive=fp, false_negative=fn, accuracy=(tp+tn)/len(records),
                  scope='fixture-v2 observed render-graph + completed content; diagnostic Supported, never effect trust',
                  records=records)
    if args.report:
        Path(args.report).write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='records'},sort_keys=True))
    if report['accuracy'] < .95 or fp or tp != sum(r['expected_supported'] for r in records):
        raise AssertionError(f'A5 criterion failed: TP={tp} TN={tn} FP={fp} FN={fn}')


if __name__ == '__main__':
    main()
