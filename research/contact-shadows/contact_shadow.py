"""Offline, picture-conditioned contact-shadow proposal. No Intel visibility code.
All adjustable effect parameters are outputs of the Referee policy. The producer
returns a signed stop layer plus shadow evidence shared with the curvature pass.
"""
from dataclasses import asdict, dataclass
import numpy as np

LUMA = np.array([.2126,.7152,.0722],dtype=np.float32)
AXES = ((1,0),(0,1),(1,1),(-1,1))


def neighbor(a, dx, dy):
    rows=np.clip(np.arange(a.shape[0])+dy,0,a.shape[0]-1)
    cols=np.clip(np.arange(a.shape[1])+dx,0,a.shape[1]-1)
    return a[rows[:,None],cols]


@dataclass(frozen=True)
class Settings:
    gain: float
    reach: float               # radius as a fraction of picture height
    far_reach_ratio: float
    sample_axes: int
    broad_fraction: float
    diagonal_weight: float
    continuity: float
    chroma_tolerance: float
    tile_quantile: float
    contact_cap: float
    peer_quantile: float
    denoise: float
    history: float


class Referee:
    """CPU research controller. Unavailable GPU timers are kept unknown."""
    def __init__(self):
        self.settings=None
        self.previous=None

    def reset(self):
        self.settings=None
        self.previous=None

    def choose(self,motion,texture,depth_support,dt,gpu_ms,allowance_ms,frame_ms):
        pressure=0 if gpu_ms is None else np.clip(gpu_ms/allowance_ms-.55,0,1)
        if frame_ms is not None:
            pressure=max(pressure,np.clip((frame_ms-18)/7,0,1))
        # Percentiles are in screen heights/second and log-luminance stops.
        moving=1. if motion is None else float(np.clip(motion/.38,0,1))
        busy=float(np.clip(texture/.45,0,1))
        trust=float(np.clip(depth_support,0,1))
        target=Settings(
            gain=(.55+.45*trust)*(1-.32*moving)*(1-.25*busy),
            reach=(.013+.016*(1-busy))*(1-.45*moving)*(1-.32*pressure),
            far_reach_ratio=1.6+.6*(1-moving)*(1-busy),
            sample_axes=2 if pressure>(.30 if self.settings and self.settings.sample_axes==2 else .60) else 4,
            broad_fraction=(.22+.34*(1-busy))*(1-.85*moving)*(1-.75*pressure),
            diagonal_weight=(.45+.55*trust)*(1-.6*moving)*(1-pressure),
            continuity=.16+.14*(1-moving)*(1-busy),
            chroma_tolerance=.055+.10*(1-busy)*(1-.55*moving),
            tile_quantile=.60+.06*trust*(1-moving),
            contact_cap=.34+.16*trust*(1-moving)*(1-pressure),
            peer_quantile=.48+.23*trust*(1-.6*moving),
            denoise=.18+.45*busy+.15*moving,
            history=(.70-.60*moving)*(1-.35*busy))
        if self.settings:
            alpha=1-np.exp(-min(dt,.25)/.18)
            prior=asdict(self.settings)
            target=Settings(**{k:v if k=='sample_axes' else float(prior[k]+alpha*(v-prior[k]))
                               for k,v in asdict(target).items()})
        self.settings=target
        return target

    def evaluate(self,rgb,depth,reversed_depth,motion_px=None,dt=1/60,dial=1.,
                 gpu_ms=None,allowance_ms=1.5,frame_ms=None,cut=False,adjacent=False,
                 mean_cap=.010,tile_cap=.040,need_shared_guard=False):
        rgb=np.asarray(rgb,np.float32)
        depth=np.asarray(depth,np.float32)
        if depth.ndim!=2 or rgb.shape!=(*depth.shape,3) or min(depth.shape)<8:
            raise ValueError("Expected a registered linear RGB image and depth plane")
        z=np.zeros_like(depth)
        def off(reason):
            self.reset()
            return z.copy(),z.copy(),dict(state=reason)
        if cut or not adjacent: self.previous=None
        if cut: self.reset()
        if not np.isfinite([dial,dt,allowance_ms,mean_cap,tile_cap]).all() or dt<=0 or allowance_ms<=0:
            return off("no reliable input")
        if not 0<=dial<=1 or min(mean_cap,tile_cap)<=0: return off("off")
        if dial==0 and not need_shared_guard: return off("off")
        if ((gpu_ms is not None and (not np.isfinite(gpu_ms) or gpu_ms>allowance_ms))
                or (frame_ms is not None and (not np.isfinite(frame_ms) or frame_ms>=25))):
            return off("time allowance")
        if reversed_depth not in (0,1): return off("no reliable depth convention")
        q=depth if reversed_depth else 1-depth
        valid=(q>0)&(q<1)&np.isfinite(q)&np.isfinite(rgb).all(axis=2)&(rgb.min(axis=2)>=0)
        if valid.sum()<256: return off("no reliable scene input")
        q=np.where(valid,q,0)
        span=float(np.quantile(q[valid],.9)-np.quantile(q[valid],.1))
        if span<2e-6: return off("insufficient depth variation")
        y=np.where(valid,rgb@LUMA,0)
        logy=np.log2(np.maximum(y,1e-6))
        # A deep-black pixel provides no usable material or light comparison.
        scene_key=float(np.median(y[valid]))
        reliable=valid & (y>max(.0001,.025*scene_key))
        chroma=np.where(valid[...,None],rgb,0)/np.maximum(np.where(valid,rgb.sum(axis=2),0)[...,None],1e-6)
        dx=np.abs(logy-neighbor(logy,1,0))
        texture=float(np.quantile(dx[reliable],.7)) if reliable.any() else 0.
        speed=None
        coverage=0.
        previous_coords=None
        if motion_px is not None:
            motion_px=np.asarray(motion_px,np.float32)
            if motion_px.shape!=(*depth.shape,2): return off("unregistered motion")
            h,w=q.shape
            yy,xx=np.indices((h,w))
            # Invalid finite sentinels can be much larger than the viewport.
            safe=np.where(np.isfinite(motion_px),motion_px,0)
            within=np.isfinite(motion_px).all(axis=2) & (np.abs(safe).max(axis=2)<=.25*max(w,h))
            px=np.rint(xx+np.clip(safe[...,0],-w,w)).astype(int)
            py=np.rint(yy+np.clip(safe[...,1],-h,h)).astype(int)
            usable=within&(px>=0)&(px<w)&(py>=0)&(py<h)&valid
            coverage=float(usable.sum()/valid.sum())
            if coverage>=.50 and usable.sum()>=256:
                speed=float(np.percentile(np.linalg.norm(safe[usable],axis=1),75)/(h*dt))
                previous_coords=(np.clip(px,0,w-1),np.clip(py,0,h-1),usable)
        settings=self.choose(speed,texture,valid.mean(),dt,gpu_ms,allowance_ms,frame_ms)
        near,credit,peers=contact_band(q,logy,chroma,reliable,span,settings,1.)
        broad,bcredit,bpeers=contact_band(q,logy,chroma,reliable,span,settings,settings.far_reach_ratio)
        fraction=settings.broad_fraction
        # Two overlapping measurements describe one contact. Blend their estimates.
        deficit=(1-fraction)*near+fraction*broad
        guard=np.maximum(credit,bcredit)
        if dial==0:
            self.previous=None
            return z.copy(),guard,dict(state="off",settings=asdict(settings),history_pixels=0)
        # Filter only between eligible receivers. The raw deficit is a hard ceiling:
        # a zero-debt crease cannot inherit a neighbor's shadow.
        smooth=np.zeros_like(q); total=np.zeros_like(q)
        for xx,yy in ((1,0),(-1,0),(0,1),(0,-1)):
            weight=neighbor(reliable,xx,yy)*np.exp(-np.abs(neighbor(q,xx,yy)-q)/max(span*.025,1e-6))
            smooth+=weight*neighbor(deficit,xx,yy); total+=weight
        proposed=(1-settings.denoise)*deficit+settings.denoise*smooth/np.maximum(total,1)
        delta=-np.minimum(deficit,proposed)*settings.gain*dial
        used_history=0
        if self.previous is not None and previous_coords is not None and self.previous[0].shape==q.shape:
            previous,pq,pl=self.previous
            px,py,usable=previous_coords
            accept=usable&reliable&(np.abs(pq[py,px]-q)<max(1e-6,.015*span))&(np.abs(pl[py,px]-logy)<.12)&(deficit>0)
            # History may steady a new shadow, never deepen the current request.
            delta=np.where(accept,np.maximum(delta,(1-settings.history)*delta+settings.history*previous[py,px]),delta)
            used_history=int(accept.sum())
        delta=np.where(np.abs(delta)>1e-6,delta,0)
        delta,stats=guard_budget(delta,y,valid,mean_cap,tile_cap)
        self.previous=(delta.copy(),q.copy(),logy.copy()) if previous_coords is not None else None
        stats.update(state="measured" if np.any(delta<0) else "no reliable response",
                     motion_heights_per_second=speed,motion_coverage=coverage,settings=asdict(settings),dial=dial,
                     peer_tiles=peers+bpeers,history_pixels=used_history,gpu_ms=gpu_ms)
        return delta,guard,stats


def contact_band(q,light,chroma,valid,span,settings,mult):
    h,w=q.shape
    radius=max(2,h*settings.reach*mult)
    tile=max(4,round(h/16))
    yy,xx=np.indices(q.shape)
    group=(xx//tile+yy//tile)%2
    tile_id=(yy//tile)*((w+tile-1)//tile)+xx//tile
    geometry=np.zeros_like(q); observed=np.zeros_like(q); openness=np.zeros_like(q)
    for dx,dy in AXES[:settings.sample_axes]:
        diag=bool(dx and dy)
        if diag and settings.diagonal_weight<.01: continue
        # Interpolate neighboring integer radii as reach changes.
        r=max(2.,radius/np.hypot(dx,dy))
        step=int(np.floor(r))
        def sample(a,sign,dist):
            k=int(np.floor(dist)); t=dist-k
            return (1-t)*neighbor(a,sign*dx*k,sign*dy*k)+t*neighbor(a,sign*dx*(k+1),sign*dy*(k+1))
        sides=[]; supports=[]; lights=[]
        for sign in (-1,1):
            end=sample(q,sign,r)-q
            mid=sample(q,sign,r*.5)-q
            # Isolated depth steps fail this test. A crease has two continuous sides.
            noise=max(span*.001,3e-7)
            continuity=np.clip(1-np.abs(mid-.5*end)/(settings.continuity*np.abs(end)+noise),0,1)
            c=sample(chroma,sign,r)
            match=np.clip(1-np.linalg.norm(c-chroma,axis=2)/settings.chroma_tolerance,0,1)
            inside=(xx+sign*dx*(step+1)>=0)&(xx+sign*dx*(step+1)<w)&(yy+sign*dy*(step+1)>=0)&(yy+sign*dy*(step+1)<h)
            ok=(sample(valid.astype('f4'),sign,r)>.999)&(sample(valid.astype('f4'),sign,r*.5)>.999)
            supports.append(continuity*match*inside*ok)
            sides.append(end); lights.append(sample(light,sign,r))
        sag=np.where(np.minimum(sides[0],sides[1])>0,sides[0]+sides[1],0)
        g=(sag/(np.abs(sides[0])+np.abs(sides[1])+max(span*.004,1e-6)) *
           np.minimum(supports[0],supports[1])*valid*(settings.diagonal_weight if diag else 1))
        brighter=np.minimum(lights[0],lights[1]) # both sides must corroborate an existing dip
        take=g>geometry
        observed=np.where(take,np.maximum(0,brighter-light),observed)
        openness=np.where(take,.5*(lights[0]+lights[1]),openness)
        geometry=np.maximum(geometry,g)
    # Compare contacts of similar geometric strength and ambient brightness.
    # Learn the target from the opposite set of tiles; a seam cannot train its own target.
    active=(geometry>.08)&valid
    deficit=np.zeros_like(q); matched_tiles=0
    if active.sum()<32: return deficit,observed,0
    brightness=np.median(openness[active])
    bins=(openness>brightness).astype('i1')+2*(geometry>.52).astype('i1')
    for k in range(4):
        for half in (0,1):
            donors=active&(bins==k)&(group!=half)
            receivers=active&(bins==k)&(group==half)
            if donors.sum()<16 or not receivers.any(): continue
            # Each distinct donor tile gets one vote to limit domination by a single object.
            ids=tile_id[donors]; values=(observed[donors]/geometry[donors]).clip(0,settings.contact_cap)
            order=np.argsort(ids); ids,values=ids[order],values[order]
            breaks=np.flatnonzero(np.diff(ids))+1
            votes=[np.quantile(v,settings.tile_quantile) for v in np.split(values,breaks) if len(v)>=3]
            if len(votes)<3: continue
            target=float(np.quantile(votes,settings.peer_quantile))
            deficit[receivers]=np.maximum(0,target*geometry[receivers]-observed[receivers])
            matched_tiles+=len(votes)
    return deficit,observed,matched_tiles


def guard_budget(delta,lum,valid,mean_cap,tile_cap):
    h,w=lum.shape; tile=max(1,round(h/16))
    before=lum[valid]
    # Protect existing near-black pixels and prevent creation of any new ones.
    black=.005
    permitted=np.maximum(0,np.log2(np.maximum(lum,1e-8)/black))
    delta=np.where(valid,np.maximum(np.minimum(delta,0),-permitted),0).astype('f4')
    crossing=(lum>=black)&(lum*np.exp2(delta)<black)
    delta[crossing]=0
    def regions(layer):
        pads=((0,(-h)%tile),(0,(-w)%tile))
        padded=np.pad(-layer,pads)
        counts=np.pad(valid,pads)
        def sums(arr):
            H,W=arr.shape
            return arr.reshape(H//tile,tile,W//tile,tile).sum(axis=(1,3))
        return sums(padded)/np.maximum(sums(counts),1)
    initial=regions(delta)
    local=np.minimum(1,tile_cap/np.maximum(initial,1e-10))
    # Never brighten. Hard caps override any smoothing or inferred preference.
    delta*=np.repeat(np.repeat(local,tile,axis=0),tile,axis=1)[:h,:w]
    delta*=min(1,mean_cap/max(float(-delta[valid].mean()),1e-10))
    def span(v):
        lo,hi=np.quantile(v,[.1,.9])
        return max(1e-6,float(np.log2(max(hi,1e-6)/max(lo,1e-6))))
    base=span(before)
    after=before*np.exp2(delta[valid])
    ratio=span(after)/base
    if not .92<=ratio<=1.10:
        delta.fill(0);after=before;ratio=1.
    return delta,dict(mean_stops=float(-delta[valid].mean()),worst_tile_stops=float(regions(delta).max()),
                       near_black_before=float((before<black).mean()),near_black_after=float((after<black).mean()),
                       contrast_ratio=ratio,affected=int((delta<-.001).sum()))
