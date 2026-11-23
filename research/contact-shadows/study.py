"""Replay registered captured DLAA inputs offline, and render actual measured A/B previews."""
import argparse
import json
import subprocess
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from contact_shadow import Referee, Settings, guard_budget, LUMA

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"camera-independent-lighting"))
from lens_readout import plane, reference


def read_frame(folder,index,step):
    path=folder/f"{index:05d}-meta.json"
    m=json.loads(path.read_text(encoding="utf-8-sig"))
    if (not m["evaluation_succeeded"] or not m["frame_closed"] or m["inputs_frame"]!=m["present_closed_frame"]
          or not (m.get("depth_reversed") in (0,1) and m["dlss"]["depth_inverted"]==bool(m["depth_reversed"]))):
        raise ValueError("Unmatched frame or unverified depth direction")
    ds={p["name"]:p for p in m["planes"]}
    w,h=m["size"]
    for key,fmt in [("depth",41),("dlssout",10),("motion",16)]:
        d=ds[key]
        if (d["width"], d["height"], d["dxgi_format"]) != (w,h,fmt): raise ValueError("Unsupported planes")
    def fetch(key,dtype,channels):
        raw=plane(path.with_name(f"{index:05d}-"),ds[key],dtype,channels)
        return raw[step//2::step, step//2::step].astype("f4")
    rgb=fetch("dlssout","<f2",4)[...,:3]
    depth=fetch("depth","<f4",1)[...,0]
    motion=None if m["dlss"]["mv_jittered"] else fetch("motion","<f4",2)*np.asarray(m["mv_scale"],"f4")/step
    return m,rgb,depth,motion


def identity(m):
    return (m["pid"],m["history_generation"],m["view"]["color"],m["view"]["depth"],tuple(m["size"]),m["depth_reversed"])


def display(c,exposure):
    v=np.maximum(0,np.nan_to_num(c))*exposure
    v=v/(1+v)
    return Image.fromarray(np.uint8(255*np.clip(np.where(v<=.0031308,12.92*v,1.055*v**(1/2.4)-.055),0,1)))


def reference_visibility(exe,base,z,m):
    w,h=z.shape[1],z.shape[0]
    matrix=np.array(m["camera"]["vp_current_row"]).reshape(4,4).T.ravel()
    desc=base.with_suffix(".txt"); data=base.with_suffix(".depth"); dest=base.with_suffix(".visibility")
    desc.write_text(" ".join(map(str,[w,h,0 if m["depth_reversed"] else 1,.5,*matrix])))
    z.astype("<f4").tofile(data)
    run=subprocess.run([str(exe),"--export-visibility",str(desc),str(data),str(dest)],capture_output=True)
    if run.returncode: raise RuntimeError(run.stdout.decode(errors="replace")+run.stderr.decode(errors="replace"))
    v=np.fromfile(dest,"<f4").reshape(h,w)
    if not np.all(np.isfinite(v)) or v.min()<-.001 or v.max()>1.001:raise ValueError("Invalid reference output")
    return np.clip(v,1e-6,1)


def preview(path,rgb,delta,base,caption):
    h,w=delta.shape
    y=rgb@LUMA
    exposure=.30/max(float(np.median(y[y>1e-5])),1e-4)
    # Choose the same region for each column by the candidate's mean shadow addition.
    H=max(32,h//3);W=max(40,w//3)
    starts=[(float(-delta[i:i+H,j:j+W].mean()),i,j) for i in range(0,h-H+1,max(1,H//3))
            for j in range(0,w-W+1,max(1,W//3))]
    _,top,left=max(starts)
    try:
        font=ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",18)
        title=ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf",23)
    except OSError: font=title=ImageFont.load_default()
    layers=[("DLAA input",np.zeros_like(delta))]
    if base is not None:layers.append(("Intel XeGTAO | same limits",base))
    layers.append(("Referee contact shadows",delta))
    cw=400;ch=round(cw*h/w);zoom=round(cw*H/W)
    canvas=Image.new("RGB",(len(layers)*cw,105+ch+zoom+42),(24,27,31)); d=ImageDraw.Draw(canvas)
    d.text((10,6),caption,font=title,fill="white")
    d.text((10,40),"Full frame above; the same enlarged crop below. Actual shading strength.",font=font,fill=(205,212,221))
    for i,(name,v) in enumerate(layers):
        img=display(rgb*np.exp2(v[...,None]),exposure)
        d.text((i*cw+8,73),name,font=font,fill="white")
        small=img.resize((cw,ch),Image.Resampling.LANCZOS)
        canvas.paste(small,(i*cw,103))
        rect=(i*cw+round(left*cw/w),103+round(top*ch/h),i*cw+round((left+W)*cw/w),103+round((top+H)*ch/h))
        d.rectangle(rect,outline=(240,185,80),width=1)
        canvas.paste(img.crop((left,top,left+W,top+H)).resize((cw,zoom),Image.Resampling.LANCZOS),(i*cw,107+ch))
    d.text((8,114+ch+zoom),"Crop selected by greatest mean added contact shadow. Common exposure for all panels.",font=font,fill=(205,212,221))
    canvas.save(path)


class FrozenReplay(Referee):
    def __init__(self, settings):
        super().__init__()
        self.fixed_settings=settings

    def choose(self, *args):
        return self.fixed_settings


def fixed_still(path):
    if path is None: return None
    rows=json.loads(path.read_text())["frames"]
    candidates=[f for f in rows if f.get("settings") and f.get("motion_heights_per_second") is not None]
    if not candidates: raise ValueError("Still reference contains no qualified motion measurement")
    still=min(candidates,key=lambda f:f["motion_heights_per_second"])
    if still["motion_heights_per_second"]>.01:
        raise ValueError("Still reference exceeds 1% of a screen height per second")
    return FrozenReplay(Settings(**still["settings"]))


def run(folder,out,step,count,baseline_exe=None,frozen_from=None):
    out.mkdir(parents=True,exist_ok=True)
    scope=None; last=None; controller=Referee(); frames=[]
    selected=None
    frozen=fixed_still(frozen_from)
    for i in range(count):
        try: m,rgb,z,motion=read_frame(folder,i,step)
        except (KeyError,OSError,ValueError) as err:
            controller.reset();last=None;frames.append(dict(index=i,state=str(err)));continue
        key=identity(m);p=m["present_ordinal"]
        linked=last is not None and scope==key and p==last+1
        d,g,stat=controller.evaluate(rgb,z,m["depth_reversed"],motion,cut=m["reset"] or m["camera_cut"] or key!=scope,adjacent=linked)
        fixed_layer=None
        if frozen is not None:
            fixed_layer,_,fixed=frozen.evaluate(rgb,z,m["depth_reversed"],motion,cut=m["reset"] or m["camera_cut"] or key!=scope,adjacent=linked)
            stat["frozen"]=fixed
            stat["mean_absolute_adaptive_vs_frozen_stops"]=float(np.abs(d-fixed_layer).mean())
        stat.update(index=i,present=p,reference=reference(m) is not None)
        frames.append(stat)
        if selected is None or (reference(m) is not None,stat.get("affected",0))>selected[0]:
            selected=((reference(m) is not None,stat.get("affected",0)),m,rgb,z,d,g,fixed_layer)
        scope=key;last=p
    result=dict(folder=folder.name,step=step,dt_assumed=1/60,frames=frames)
    if selected:
        _,m,rgb,z,d,g,fixed_layer=selected; baseline=None
        if reference(m) is not None and baseline_exe:
            v=reference_visibility(baseline_exe,out/"intel",z,m)
            q=z if m["depth_reversed"] else 1-z
            mask=(q>0)&(q<1)&np.isfinite(rgb).all(axis=2)&(rgb.min(axis=2)>=0)
            baseline,metrics=guard_budget(np.log2(v),rgb@LUMA,mask,.010,.040)
            weights=baseline[mask]; target=d[mask]
            scale=float(np.dot(weights,target)/max(np.dot(weights,weights),1e-12))
            residual=float(np.linalg.norm(target-scale*weights)/max(np.linalg.norm(target),1e-9))
            result["intel"]=dict(settings="repository High, radius 0.5 scene units, one spatial denoise, no temporal history",
                                best_scalar=scale,relative_residual_after_best_scalar=residual,**metrics)
        if frozen_from: result["frozen_from"]=str(frozen_from)
        np.savez_compressed(out/"selected-layers.npz",delta_stops=d,existing_shadow_stops=g,
                            **({"frozen_stops":fixed_layer} if fixed_layer is not None else {}),
                            **({"intel_budgeted_stops":baseline} if baseline is not None else {}))
        result["selected_frame"]=m["index"]
        preview(out/"comparison.png",rgb,d,baseline,m["exe"]+"   /   frame "+str(m["index"]))
    (out/"metrics.json").write_text(json.dumps(result,indent=2,allow_nan=False))
    return result

if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("folder",type=Path);p.add_argument("out",type=Path)
    p.add_argument("--step",type=int,default=4);p.add_argument("--count",type=int,default=30);p.add_argument("--intel-exe",type=Path)
    p.add_argument("--frozen-from",type=Path,help="metrics.json from a still run of the same scene")
    a=p.parse_args();run(a.folder,a.out,a.step,a.count,a.intel_exe,a.frozen_from)
