"""Offline candidate search and fixed-frame holdout. No calibration probes are drawn to the game."""
import argparse
from dataclasses import asdict, replace
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import numpy as np

from contact_shadow import LUMA, neighbor, guard_budget
from form_lighting import FormReferee, Budget, box
from referee_tuning import AdaptiveForm, SceneReferee, observe, propose_ao, propose_shape, quality
from study import read_frame, reference, identity


# Multipliers are local search steps relative to the picture-dependent starting proposal.
AO_MOVES={
    "radius":(.65,1.45), "radius_multiplier":(.8,1.25), "falloff":(.75,1.22),
    "sample_distribution":(.8,1.18), "thin_occluder":(.4,1.7),
    "depth_mip_offset":(.76,1.28), "denoise":(-3,-1,1),
    "final_power":(.75,1.3), "amount":(.82,1.13)}
SHAPE_MOVES={n:(.82,1.19) for n in ("edge","haze","guard","reach","wide","dark","lift")}
LIMITS={"amount":(0,100),"denoise":(0,3),"radius":(.01,100),"radius_multiplier":(.3,3),
        "falloff":(.001,1),"sample_distribution":(.1,3),"thin_occluder":(0,1),
        "final_power":(.01,5),"depth_mip_offset":(0,30),"temporal":(0,.95)}


def modify(base, changes, intel):
    values={}
    for name, step in changes.items():
        start=getattr(base,name)
        value=start+step if name=="denoise" else start*step
        low,high=LIMITS[name] if intel else (.55,1.55)
        value=float(np.clip(value,low,high))
        values[name]=round(value) if name in ("amount","denoise") else value
    return replace(base,**values)


def projection(meta):
    ref=reference(meta)
    if ref is None: return None
    m=np.asarray(meta["camera"]["vp_current_row"]).reshape(4,4).T
    forward=m[3,:3]
    norm=forward@forward
    a=float(m[2,:3]@forward/norm)
    return ref[0],a,float((m[2,3]-a*m[3,3])/np.sqrt(norm))


class SavedFrame:
    def __init__(self,folder,index,step,frame_ms):
        self.m,self.rgb,self.z,self.mv=read_frame(folder,index,step)
        self.rev=self.m["depth_reversed"]
        self.picture=observe(self.rgb,self.z,self.rev,self.mv,(frame_ms/1000 if frame_ms is not None else 1/60),frame_ms,projection(self.m))
        if self.picture is None: raise ValueError("no reliable picture")
        q=self.z if self.rev else 1-self.z
        self.valid=(q>0)&(q<1)&np.isfinite(q)&np.isfinite(self.rgb).all(axis=2)&(self.rgb.min(axis=2)>=0)
        self.y=self.rgb@LUMA
        self.valid &= np.isfinite(self.y)
        self.edge=np.zeros(q.shape,bool)
        for dx,dy in ((1,0),(-1,0),(0,1),(0,-1)):
            self.edge |= np.abs(q-neighbor(q,dx,dy))>.12*np.maximum(q,1e-6)
        self.edge &= self.valid
        self.dark=(self.y<np.quantile(self.y[self.valid],.2))&self.valid
        loglight=np.log2(np.maximum(self.y,1e-6))
        rad=max(2,round(q.shape[0]*.03))
        local=box(np.where(self.valid,loglight,0),rad)/np.maximum(.001,box(self.valid.astype("f4"),rad))
        self.guard=np.exp2(-1.35*np.maximum(0,local-loglight))
        # The shape and strength Cort picked are the appearance reference at the same resolution.
        old=FormReferee()
        self.contact=old.layers(self.rgb,self.z,self.rev,self.mv,form_dial=0)[0]
        a,c,_=old.layers(self.rgb,self.z,self.rev,self.mv,reset=True)
        self.combined=a+c


def image_loss(frame, delta, target):
    mask=frame.valid
    wanted=np.abs(target[mask])
    seen=np.abs(delta[mask])
    coverage=float((seen>=.25).mean())
    reference_coverage=float((wanted>=.25).mean())
    # Do not let sky, or many zero-valued flat pixels, outvote visibly affected surfaces.
    active=mask&(np.abs(target)>=.08)
    outside=mask&(np.abs(target)<.04)
    def mean(v,region):
        return float(np.mean(v[region])) if region.any() else 0.
    miss=np.maximum(0,np.abs(target-delta)-(.055+.12*np.abs(target)))
    overshoot=np.maximum(np.abs(delta)-np.abs(target)-.15,0)
    halo=np.maximum(np.abs(delta)-.40,0)
    cost=(.16*mean(halo,frame.edge)+.10*mean(np.maximum(-delta-.25,0),frame.dark)+mean(miss,active)+.40*mean(np.abs(delta),outside)+
          .85*abs(coverage-reference_coverage)+.30*mean(overshoot,frame.edge)+
          .25*mean(np.maximum(target-delta,0),frame.dark))
    return cost,coverage


def spatial_gpu(frame,opt,exe,directory,previous=None):
    path=Path(directory)
    h,w=frame.z.shape
    def inputs(f,settings,tag,keep=None):
        mat=np.asarray(f.m["camera"]["vp_current_row"]).reshape(4,4).T.flatten()
        args=[w,h,0 if f.rev else 1,settings.radius,*mat,settings.quality,settings.denoise,settings.amount,
              settings.radius_multiplier,settings.falloff,settings.sample_distribution,settings.thin_occluder,
              settings.final_power,settings.depth_mip_offset]
        if keep is not None:
            jitter=np.asarray(f.m["jitter_px"],dtype=float)*[w/f.m["size"][0], h/f.m["size"][1]]
            args.extend([keep,*jitter])
        header=path/(tag+".txt")
        depth=path/(tag+".depth")
        header.write_text(" ".join(map(str,args)))
        f.z.astype("<f4").tofile(depth)
        return [str(header),str(depth)]
    if previous is None or opt.temporal<=0:
        command=["--export-visibility",*inputs(frame,opt,"current")]
    else:
        old,old_opt=previous
        if (identity(old.m)!=identity(frame.m) or old.m["present_ordinal"]+1!=frame.m["present_ordinal"]
                or frame.m["reset"] or frame.m["camera_cut"] or frame.picture.speed is None):
            raise ValueError("invalid history link")
        # First evaluation seeds the normal GPU history with this frame's spatial estimate.
        # An out-of-range vector explicitly rejects a history tap in the shader.
        mv=np.where(np.isfinite(frame.mv).all(axis=2)[...,None] &
                    (np.abs(frame.mv).max(axis=2,keepdims=True)<.25*max(h,w)),frame.mv,2.*max(h,w))
        mv.astype("<f4").tofile(path/"motion")
        command=["--export-visibility-pair",*inputs(old,old_opt,"previous",.5),
                 *inputs(frame,opt,"current",opt.temporal),str(path/"motion")]
    run=subprocess.run([str(exe),*command,str(path/"result")],capture_output=True)
    if run.returncode: raise RuntimeError(run.stdout.decode(errors="replace")+run.stderr.decode(errors="replace"))
    vis=np.fromfile(path/"result",dtype="<f4").reshape(h,w)
    if not np.isfinite(vis).all() or vis.min()<-.001 or vis.max()>1.001: raise ValueError("invalid GPU response")
    proposed=np.log2(np.clip(vis,1e-6,1))*frame.guard
    return guard_budget(proposed,frame.y,frame.valid,Budget().mean,Budget().tile)[0]


def history_loss(old,current,old_delta,new_delta):
    """Visible residual on stable surfaces and disagreement on newly exposed pixels."""
    h,w=current.z.shape
    yy,xx=np.indices((h,w))
    jitter=(np.asarray(old.m["jitter_px"])-np.asarray(current.m["jitter_px"]))*[w/current.m["size"][0],h/current.m["size"][1]]
    mv=np.nan_to_num(current.mv,nan=2*w,posinf=2*w,neginf=2*w)
    x=np.rint(xx+np.clip(mv[...,0],-2*w,2*w)+jitter[0]).astype(int)
    y=np.rint(yy+np.clip(mv[...,1],-2*h,2*h)+jitter[1]).astype(int)
    inside=(x>=0)&(x<w)&(y>=0)&(y<h)
    x=np.clip(x,0,w-1); y=np.clip(y,0,h-1)
    q=current.z if current.rev else 1-current.z
    same_depth=np.abs(old.z[y,x]-current.z)<.015*np.maximum(q,1e-6)
    same_light=np.abs(np.log2(np.maximum(old.y[y,x],1e-6)/np.maximum(current.y,1e-6)))<.16
    stable=inside&same_depth&same_light&current.valid&old.valid[y,x]
    revealed=current.valid&~stable
    metric=image_loss(current,new_delta,current.contact)[0]
    if stable.any():
        metric+=.8*float(np.mean(np.abs(new_delta[stable]-old_delta[y,x][stable])))
    if revealed.any():
        metric+=.4*float(np.mean(np.abs(new_delta[revealed]-current.contact[revealed])))
    return metric


def coordinate_search(base,moves,evaluate,intel):
    changes={}
    incumbent=evaluate(base)
    trials=[dict(control="start",score=incumbent)]
    for name,steps in moves.items():
        best=incumbent
        selection=None
        for step in steps:
            attempt={**changes,name:step}
            settings=modify(base,attempt,intel)
            score=evaluate(settings)
            trials.append(dict(control=name,value=getattr(settings,name),score=score))
            if score < best-1e-5:
                best,selection=score,step
        if selection is not None:
            changes[name]=selection
            incumbent=best
    return changes,trials


def run(folder,out,exe,step,frame_ms,external=None):
    first=SavedFrame(folder,0,step,frame_ms)
    sets={"shape":{},"ao":{}}
    trials={}
    with TemporaryDirectory() as directory:
        def render_shape(frame,shape,brain=None,reset=True):
            if brain is None: brain=AdaptiveForm()
            brain.request=shape
            a,c,s=brain.layers(frame.rgb,frame.z,frame.rev,frame.mv,reset=reset,frame_ms=frame_ms,dt=frame_ms/1000 if frame_ms is not None else 1/60)
            return a+c,s
        if external is None:
            sets["shape"],trials["shape"]=coordinate_search(propose_shape(first.picture),SHAPE_MOVES,
                lambda v:image_loss(first,render_shape(first,v)[0],first.combined)[0],False)
            initial=propose_ao(first.picture,False)
            if initial is not None:
                sets["ao"],trials["ao"]=coordinate_search(initial,AO_MOVES,
                    lambda v:image_loss(first,spatial_gpu(first,v,exe,directory),first.contact)[0],True)
        else:
            sets=json.loads(external.read_text())["adjustments"]
        if external is None and trials.get("ao"):
            second=SavedFrame(folder,1,step,frame_ms)
            available=(second.picture.depth_units is not None and second.picture.speed is not None and
                       identity(first.m)==identity(second.m) and not second.m["camera_cut"] and not second.m["reset"] and
                       second.m["present_ordinal"]==first.m["present_ordinal"]+1)
            if available:
                old_opt=modify(propose_ao(first.picture,False),sets["ao"],True)
                old_layer=spatial_gpu(first,old_opt,exe,directory)
                new_opt=modify(propose_ao(second.picture,True),sets["ao"],True)
                def rate_keep(candidate):
                    layer=spatial_gpu(second,candidate,exe,directory,(first,old_opt))
                    return history_loss(first,second,old_layer,layer)
                steps,tested=coordinate_search(new_opt,{"temporal":(0.,.6,1.18)},rate_keep,True)
                sets["ao"].update(steps)
                trials["history"]=tested
        validation={}
        if external is None:
            # Validate a correction against the uncorrected scene-derived rule on other frames.
            # Keep the analytic rule when a fitted correction cannot repeat its benefit.
            for kind in ("shape","ao"):
                if not sets[kind]: continue
                scored=[]
                for idx in (4,13,21):
                    f=SavedFrame(folder,idx,step,frame_ms)
                    if kind=="ao":
                        base=propose_ao(f.picture,False)
                        if base is None: continue
                        def loss(option):
                            return image_loss(f,spatial_gpu(f,option,exe,directory),f.contact)[0]
                    else:
                        base=propose_shape(f.picture)
                        def loss(option):
                            return image_loss(f,render_shape(f,option)[0],f.combined)[0]
                    scored.append([idx,loss(base),loss(modify(base,sets[kind],kind=="ao"))])
                if kind=="ao":
                    # A static validation frame says nothing about temporal keep.
                    history={k:v for k,v in sets[kind].items() if k=="temporal"}
                else: history={}
                keep=bool(scored and np.mean([r[2]-r[1] for r in scored]) < -1e-5)
                validation[kind]=dict(frames=scored,spatial_fit_retained=keep)
                if not keep: sets[kind]=history
            if "temporal" in sets["ao"]:
                scored=[]
                for idx in range(3,24):
                    old=SavedFrame(folder,idx-1,step,frame_ms)
                    new=SavedFrame(folder,idx,step,frame_ms)
                    if (old.picture.depth_units is None or new.picture.depth_units is None or
                        new.picture.speed is None or new.m["camera_cut"] or new.m["reset"] or
                        identity(old.m)!=identity(new.m) or new.m["present_ordinal"]!=old.m["present_ordinal"]+1):
                        continue
                    opt=modify(propose_ao(old.picture,False),sets["ao"],True)
                    old_layer=spatial_gpu(old,opt,exe,directory)
                    ordinary=propose_ao(new.picture,True)
                    new_opt=modify(ordinary,sets["ao"],True)
                    def score(candidate):
                        layer=spatial_gpu(new,candidate,exe,directory,(old,opt))
                        return history_loss(old,new,old_layer,layer)
                    scored.append([idx,score(replace(new_opt,temporal=ordinary.temporal)),score(new_opt)])
                    if len(scored)==2: break
                keep=bool(scored and np.mean([r[2]-r[1] for r in scored]) < -1e-5)
                validation["history"]=dict(frames=scored,fit_retained=keep)
                if not keep: del sets["ao"]["temporal"]
        ref_contact,ref_combined=FormReferee(),FormReferee()
        analytic_shape=AdaptiveForm()
        analytic_scene=SceneReferee()
        shape=AdaptiveForm()
        scene=SceneReferee()
        rows=[]
        previous=None
        last_key=None
        last_present=None
        for index in range(30):
            f=first if index==0 else SavedFrame(folder,index,step,frame_ms)
            key=identity(f.m)
            cut=key!=last_key or f.m["reset"] or f.m["camera_cut"]
            history_ok=not cut and previous is not None and last_present is not None and f.m["present_ordinal"]==last_present+1
            dt=frame_ms/1000 if frame_ms is not None else 1/60
            f.contact=ref_contact.layers(f.rgb,f.z,f.rev,f.mv,form_dial=0,dt=dt,reset=cut,frame_ms=frame_ms)[0]
            ra,rc,_=ref_combined.layers(f.rgb,f.z,f.rev,f.mv,dt=dt,reset=cut,frame_ms=frame_ms)
            f.combined=ra+rc
            plain_shape=render_shape(f,propose_shape(f.picture),analytic_shape,cut)[0]
            requested=modify(propose_shape(f.picture),sets["shape"],False)
            combined,s=render_shape(f,requested,shape,cut)
            score,coverage=image_loss(f,combined,f.combined)
            row=dict(index=index,picture=asdict(f.picture),form=asdict(shape.current) if shape.current else None,
                     applied_form_controls=s.get("controls"),shape_loss=score,shape_coverage=coverage,
                     contrast=s.get("contrast"),mean_dark=s.get("mean_dark"),tile_dark=s.get("tile_dark"),
                     black_before=s.get("near_black_before"),black_after=s.get("near_black_after"))
            base=propose_ao(f.picture,history_ok)
            opt=scene.apply(modify(base,sets["ao"],True) if base else None,frame_ms/1000 if frame_ms is not None else 1/60,cut)
            analytic=analytic_scene.apply(base,dt,cut)
            row["ao_controls"]=asdict(opt) if opt else None
            if opt is not None:
                link=previous if history_ok and opt.temporal>0 else None
                delta=spatial_gpu(f,opt,exe,directory,link)
                row["history_pair_rendered"]=link is not None
                row["ao_loss"],row["ao_coverage"]=image_loss(f,delta,f.contact)
                if index in (0,29):
                    row["untuned_ao_loss"]=image_loss(f,spatial_gpu(f,analytic,exe,directory,(previous_plain if history_ok and analytic.temporal>0 else None)),f.contact)[0]
            if index in (0,29):
                row["untuned_shape_loss"]=image_loss(f,plain_shape,f.combined)[0]
            previous_plain=(f,analytic) if analytic is not None else None
            previous=(f,opt) if opt is not None else None
            rows.append(row)
            last_key,last_present=key,f.m["present_ordinal"]
    result=dict(capture=folder.name,step=step,frame_ms=frame_ms,history_render="two actual GPU evaluations for each qualified adjacent pair; longer chains unscored",
                adjustments=sets,trials=trials,validation=validation,frames=rows)
    out.parent.mkdir(exist_ok=True,parents=True)
    out.write_text(json.dumps(result,indent=2,allow_nan=False))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture",type=Path)
    parser.add_argument("report",type=Path)
    parser.add_argument("--gpu-fixture",type=Path,required=True)
    parser.add_argument("--step",type=int,default=4)
    parser.add_argument("--frame-ms",type=float,help="Known present interval for a controlled replay")
    parser.add_argument("--calibration",type=Path,help="Apply prior settings corrections to a held-out recording")
    args=parser.parse_args()
    run(args.capture,args.report,args.gpu_fixture,args.step,args.frame_ms,args.calibration)
