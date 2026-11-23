"""Controlled present-interval test on a fixed qualified capture, with actual XeGTAO dispatches."""
from argparse import ArgumentParser
from dataclasses import replace
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import numpy as np

from referee_tuning import SceneReferee, AdaptiveForm, propose_ao, propose_shape
from tune_replay import SavedFrame, modify, spatial_gpu


def trial(folder,exe,fit):
    f=SavedFrame(folder,0,4,None)
    state=SceneReferee()
    form=AdaptiveForm()
    rows=[]
    layers=[]
    with TemporaryDirectory() as temp:
        for ms in (16.,20.,20.01,22.,20.):
            evidence=replace(f.picture,frame_ms=ms)
            opt=state.apply(modify(propose_ao(evidence,False),fit["ao"],True),ms/1000)
            delta=spatial_gpu(f,opt,exe,temp)
            form.request=modify(propose_shape(evidence),fit["shape"],False)
            a,c,s=form.layers(f.rgb,f.z,f.rev,f.mv,frame_ms=ms,dt=ms/1000)
            rows.append(dict(frame_ms=ms,fps=1000/ms,quality=opt.quality,directions=s["controls"]["directions"],
                             mean_shading_change_vs_ultra=float(np.abs(delta-layers[0]).mean()) if layers else 0.,
                             form_mean_absolute=s["mean_absolute"]))
            layers.append(delta)
    assert [r["quality"] for r in rows]==[3,3,0,0,3]
    assert [r["directions"] for r in rows]==[4,4,2,2,4]
    np.testing.assert_array_equal(layers[1], layers[0])
    assert np.abs(layers[2]-layers[1]).max()>.001, "both GPU quality levels produced identical shading"
    np.testing.assert_array_equal(layers[-1],layers[0])
    return rows


if __name__=="__main__":
    parser=ArgumentParser(description=__doc__)
    parser.add_argument("capture",type=Path)
    parser.add_argument("calibration",type=Path)
    parser.add_argument("gpu_fixture",type=Path)
    parser.add_argument("output",type=Path)
    args=parser.parse_args()
    rows=trial(args.capture,args.gpu_fixture,json.loads(args.calibration.read_text())["adjustments"])
    args.output.write_text(json.dumps(rows,indent=2))
