"""Report temporal changes in custom lighting on matched, adjacent saved scene pixels."""
import argparse
import json
from pathlib import Path

import numpy as np

from referee_tuning import AdaptiveForm, propose_shape
from study import identity
from tune_replay import SavedFrame, modify, stable_surface_samples


def measure(folder, fit, step=4):
    referee = AdaptiveForm()
    previous = None
    pairs = []
    errors = [[], []]
    for index in range(30):
        current = SavedFrame(folder, index, step, None)
        meta = current.m
        reset = (previous is None or identity(meta) != identity(previous.m)
                 or meta["present_ordinal"] != previous.m["present_ordinal"] + 1
                 or meta["reset"] or meta["camera_cut"])
        referee.request = modify(propose_shape(current.picture), fit, False)
        contact, form, _ = referee.layers(current.rgb, current.z, current.rev, current.mv, reset=reset)
        if not reset and current.mv is not None:
            y, x, valid = stable_surface_samples(previous, current)
            valid &= np.isfinite(current.mv).all(axis=2)
            if valid.any():
                pairs.append(dict(first=previous.m["present_ordinal"], last=meta["present_ordinal"],
                                  matched=int(valid.sum()), scene=int(current.valid.sum())))
                for samples, old, new in zip(errors, layers, (contact, form)):
                    samples.append(np.abs(new[valid] - old[y[valid], x[valid]]))
        previous = current
        layers = contact, form
    stats = {}
    for name, items in zip(("contact", "curvature"), errors):
        if items:
            values = np.concatenate(items)
            stats[name] = dict(mean=float(values.mean()), p95=float(np.percentile(values, 95)),
                               over_quarter=float((values >= .25).mean()))
    return dict(capture=folder.name, step=step, pairs=pairs, absolute_change_stops=stats)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    fits = json.loads(args.calibration.read_text())["adjustments"]["shape"]
    args.output.write_text(json.dumps(measure(args.folder, fits), indent=2, allow_nan=False) + "\n")
