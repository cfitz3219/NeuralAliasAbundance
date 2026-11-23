"""Replay a 30-frame capture or export full-resolution, exposure-matched lighting proofs."""
import argparse
import json
from pathlib import Path
import numpy as np
from form_lighting import FormReferee
from study import read_frame, identity, display, LUMA
from referee_tuning import AdaptiveForm, observe, propose_shape


def setup_adaptive(controller, calibration, rgb, depth, meta, mv, frame_ms):
    if calibration is None: return
    from tune_replay import modify
    dt=frame_ms/1000 if frame_ms is not None else 1/60
    evidence=observe(rgb,depth,meta["depth_reversed"],mv,dt,frame_ms)
    controller.request=modify(propose_shape(evidence),calibration,False)


def replay(folder, out, count, step, calibration=None, frame_ms=None):
    rows = []
    controller = AdaptiveForm() if calibration is not None else FormReferee()
    last_key = None
    for index in range(count):
        try:
            meta, rgb, depth, mv = read_frame(folder,index,step)
            key = identity(meta)
            reset = last_key!=key or meta["reset"] or meta["camera_cut"]
            setup_adaptive(controller,calibration,rgb,depth,meta,mv,frame_ms)
            _,_,stats = controller.layers(rgb,depth,meta["depth_reversed"],mv,reset=reset,frame_ms=frame_ms,dt=frame_ms/1000 if frame_ms is not None else 1/60)
            last_key=key
        except (ValueError, KeyError, OSError) as err:
            stats=dict(state="no reliable input",why=str(err))
            last_key=None
        rows.append(dict(index=index,**stats))
    (out/"replay.json").write_text(json.dumps(rows,indent=2,allow_nan=False))


def proofs(folder, out, index, calibration=None, frame_ms=None):
    meta,rgb,depth,mv = read_frame(folder,index,1)
    y = rgb@LUMA
    exposure = .3/max(float(np.median(y[y>1e-5])),1e-4)
    display(rgb,exposure).save(out/"original.png")
    measurements = {}
    for name,cd,fd in [("combined",1.,1.),("contact",1.,0.),("form",0.,1.)]:
        controller=AdaptiveForm() if calibration is not None else FormReferee()
        setup_adaptive(controller,calibration,rgb,depth,meta,mv,frame_ms)
        contact,form,stats = controller.layers(rgb,depth,meta["depth_reversed"],mv,cd,fd,reset=True,frame_ms=frame_ms,dt=frame_ms/1000 if frame_ms is not None else 1/60)
        picture = display(rgb*np.exp2((contact+form)[...,None]),exposure)
        picture.save(out/(name+".png"))
        measurements[name]=stats
    info=dict(capture=folder.name,frame=index,exposure=exposure,size=meta["size"],layers=measurements)
    (out/"native.json").write_text(json.dumps(info,indent=2,allow_nan=False))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=["replay","proofs"])
    parser.add_argument("folder", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--count",type=int,default=30)
    parser.add_argument("--step",type=int,default=4)
    parser.add_argument("--frame",type=int,default=0)
    parser.add_argument("--referee-calibration",type=Path,help="Audition fitted Referee shape controls")
    parser.add_argument("--frame-ms",type=float,help="Measured whole-frame time or an explicit test interval")
    a=parser.parse_args()
    fit=json.loads(a.referee_calibration.read_text())["adjustments"]["shape"] if a.referee_calibration else None
    a.output.mkdir(exist_ok=True,parents=True)
    if a.mode=="replay": replay(a.folder,a.output,a.count,a.step,fit,a.frame_ms)
    else: proofs(a.folder,a.output,a.frame,fit,a.frame_ms)
