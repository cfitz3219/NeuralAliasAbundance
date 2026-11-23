"""Replay a 30-frame capture or export full-resolution, exposure-matched lighting proofs."""
import argparse
import json
from pathlib import Path
import numpy as np
from form_lighting import FormReferee
from study import read_frame, identity, display, LUMA


def replay(folder, out, count, step):
    rows = []
    controller = FormReferee()
    last_key = None
    for index in range(count):
        try:
            meta, rgb, depth, mv = read_frame(folder,index,step)
            key = identity(meta)
            reset = last_key!=key or meta["reset"] or meta["camera_cut"]
            _,_,stats = controller.layers(rgb,depth,meta["depth_reversed"],mv,reset=reset)
            last_key=key
        except (ValueError, KeyError, OSError) as err:
            stats=dict(state="no reliable input",why=str(err))
            last_key=None
        rows.append(dict(index=index,**stats))
    (out/"replay.json").write_text(json.dumps(rows,indent=2,allow_nan=False))


def proofs(folder, out, index):
    meta,rgb,depth,mv = read_frame(folder,index,1)
    y = rgb@LUMA
    exposure = .3/max(float(np.median(y[y>1e-5])),1e-4)
    display(rgb,exposure).save(out/"original.png")
    measurements = {}
    for name,cd,fd in [("combined",1.,1.),("contact",1.,0.),("form",0.,1.)]:
        contact,form,stats = FormReferee().layers(rgb,depth,meta["depth_reversed"],mv,cd,fd,reset=True)
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
    a=parser.parse_args()
    a.output.mkdir(exist_ok=True,parents=True)
    if a.mode=="replay": replay(a.folder,a.output,a.count,a.step)
    else: proofs(a.folder,a.output,a.frame)
