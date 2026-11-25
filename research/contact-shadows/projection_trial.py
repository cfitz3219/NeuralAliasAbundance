"""Compare Intel XeGTAO using a measured lens and an independent reference on the same capture."""
import argparse
from dataclasses import asdict, replace
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace

import numpy as np

from referee_tuning import AO
from study import read_frame
from tune_replay import gpu_visibility, projection

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "camera-independent-lighting"))
from calibration import score_layer


def blind_matrix(fov, offset, width, height, depth_unit):
    # Reversed perspective: q + offset = depth_unit / Z. This fixture's raster Y points down.
    focal = 1 / np.tan(np.deg2rad(fov / 2))
    return [focal * height / width,0,0,0, 0,-focal,0,0, 0,0,-offset,1, 0,0,depth_unit,0]


def compare(capture, reference_file, fov, offset, fixture, step, output):
    ref = json.loads(reference_file.read_text(encoding="utf-8-sig"))
    params = projection(ref)
    if params is None or not ref["depth_reversed"]:
        raise ValueError("This trial requires a qualified reversed-depth reference projection")
    width, height = ref["size"]
    baseline = ref["camera"]["vp_current_row"]
    # Express radius as a share of the reference depth unit. The unknown unit cancels
    # if depth unpack and effective radius are rescaled together.
    trial_unit = 1.
    blind = blind_matrix(fov, offset, width, height, trial_unit)
    settings = AO(3, 2, 100, .5, 1.457, .615, 2., 0., 2.2, 3.3, 0.)
    rows = []
    with TemporaryDirectory() as tmp:
        for index in (0, 8):
            meta, rgb, depth, _ = read_frame(capture, index, step)
            if meta["size"] != ref["size"] or not meta["depth_reversed"]:
                raise ValueError("Capture and reference path mismatch")
            frame = SimpleNamespace(z=depth, rev=1, m=meta)
            mask = np.isfinite(depth) & (depth>0) & (depth<1) & np.isfinite(rgb).all(axis=2)
            reference = np.log2(gpu_visibility(frame, settings, fixture, tmp, matrix=baseline))
            # Use the same depth *units* first so absolute near-plane calibration is not scored as lens error.
            candidate = blind_matrix(fov, offset, width, height, params[2])
            predicted = np.log2(gpu_visibility(frame, settings, fixture, tmp, matrix=candidate))
            scaled = replace(settings, radius=settings.radius * trial_unit / params[2])
            unit_result = np.log2(gpu_visibility(frame, scaled, fixture, tmp, matrix=blind))
            rows.append(dict(frame=index, lens=score_layer(predicted,reference,mask),
                             arbitrary_unit=score_layer(unit_result,reference,mask)))
    result = dict(capture=capture.name, reference=reference_file.parent.name,
                  assumption="Earlier session at the same zoom", vertical_fov=fov, depth_offset=offset,
                  reference_depth_unit=params[2], step=step, controls=asdict(settings), frames=rows)
    output.write_text(json.dumps(result,indent=2))
    return result


if __name__ == "__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("capture",type=Path)
    p.add_argument("reference_meta",type=Path)
    p.add_argument("fov",type=float)
    p.add_argument("offset",type=float)
    p.add_argument("fixture",type=Path)
    p.add_argument("output",type=Path)
    p.add_argument("--step",type=int,default=1)
    a=p.parse_args()
    compare(a.capture,a.reference_meta,a.fov,a.offset,a.fixture,a.step,a.output)
