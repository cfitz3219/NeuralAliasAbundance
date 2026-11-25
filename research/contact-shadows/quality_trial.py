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
        for ms in (16.,20.,1000./41,25.,25.01,1000./15.01,1000./15,80.,25.,16.):
            evidence=replace(f.picture,frame_ms=ms)
            proposal=propose_ao(evidence,False)
            request=modify(proposal,fit["ao"],True) if proposal else None
            # Hold each measured interval until the recovery observation window has elapsed.
            for _ in range(int(np.ceil(1.05*1000/ms))):
                opt=state.apply(request,ms/1000,picture=evidence)
            delta=spatial_gpu(f,opt,exe,temp) if opt else np.zeros_like(f.z)
            form.request=modify(propose_shape(evidence),fit["shape"],False)
            a,c,s=form.layers(f.rgb,f.z,f.rev,f.mv,frame_ms=ms,dt=ms/1000)
            rows.append(dict(frame_ms=ms,fps=1000/ms,quality=opt.quality if opt else None,ao_dispatched=opt is not None,directions=s.get("controls",{}).get("directions"),
                             mean_shading_change_vs_ultra=float(np.abs(delta-layers[0]).mean()) if layers else 0.,
                             form_mean_absolute=s.get("mean_absolute")))
            layers.append(delta)
    assert [r["quality"] for r in rows]==[3,1,1,1,0,0,None,None,1,3]
    np.testing.assert_array_equal(layers[8], layers[1])
    assert not layers[6].any() and not layers[7].any()
    assert np.abs(layers[4]-layers[1]).max()>.001, "Low and Medium produced identical shading"
    assert np.abs(layers[1]-layers[0]).max()>.001, "both GPU quality levels produced identical shading"
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
