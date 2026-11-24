"""Offline Referee decisions shared by the capture audition and its replay."""
from dataclasses import asdict, dataclass, replace
import numpy as np
from contact_shadow import LUMA, neighbor
from form_lighting import Controls, FormReferee, box, broad


def unit(x):
    return float(np.clip(x,0,1))


@dataclass(frozen=True)
class Picture:
    thin: float
    detail: float
    fog: float
    existing_shadow: float
    speed: float | None
    frame_ms: float | None
    coverage: float
    depth_units: float | None
    focal: float | None


def observe(rgb,depth,rev,motion,dt,frame_ms=None,projection=None):
    """Projection, when qualified by the caller, is (focal length, a, b) for depth=a+b/Z."""
    if rev not in (0,1) or not np.isfinite(dt) or dt<=0 or rgb.shape!=(*depth.shape,3):
        return None
    q=depth if rev else 1-depth
    valid=(q>0)&(q<1)&np.isfinite(q)&np.isfinite(rgb).all(axis=2)&(rgb.min(axis=2)>=0)
    if valid.sum()<256: return None
    q=np.where(valid,q,0)
    if np.ptp(np.quantile(q[valid],[.03,.97]))<2e-6: return None
    light=np.log2(np.maximum(np.where(valid,rgb@LUMA,0),1e-6))
    h,w=q.shape
    r=max(1,round(.004*h))
    wire=np.zeros(q.shape,bool)
    for x,y in ((r,0),(0,r)):
        sides=np.minimum(q-neighbor(q,x,y),q-neighbor(q,-x,-y))
        wire |= (sides>.045*np.maximum(q,1e-6)) & neighbor(valid,x,y)&neighbor(valid,-x,-y)
    thin=unit(wire[valid].mean()/.08)
    radius=max(2,round(h*.027))
    density=box(valid.astype("f4"),radius)
    surround=box(light*valid,radius)/np.maximum(.001,density)
    residual=light-surround
    detail=unit(np.quantile(np.abs(residual[valid]),.7)/1.4)
    far=valid&(q<=np.quantile(q[valid],.25))
    fog=unit(1-np.median(np.abs(residual[far]))/.65)
    existing=unit(np.mean(np.maximum(-residual[valid],0))/.9)
    yy,xx=np.ogrid[:h,:w]
    speed=None
    if motion is not None and motion.shape==(*q.shape,2):
        ok=valid & np.isfinite(motion).all(axis=2)&(np.abs(motion).max(axis=2)<.25*max(h,w))
        ok &= (xx+motion[...,0]>=0)&(xx+motion[...,0]<w)&(yy+motion[...,1]>=0)&(yy+motion[...,1]<h)
        if ok.sum()>=max(256,.50*valid.sum()):
            speed=float(np.percentile(np.linalg.norm(motion[ok],axis=1),75)/(h*dt))
    focal=distance=None
    if projection is not None:
        f,a,b=projection
        if np.isfinite(projection).all() and f>0 and abs(b)>1e-12:
            z=depth[valid]
            with np.errstate(divide="ignore",invalid="ignore"): metric=b/(z-a)
            usable=metric[np.isfinite(metric)&(metric>0)]
            if usable.size>=.9*z.size:
                distance=float(np.quantile(usable,.40))
                focal=float(f)
    return Picture(thin,detail,fog,existing,speed,frame_ms,float(valid.mean()),distance,focal)


@dataclass(frozen=True)
class AO:
    quality: int
    denoise: int
    amount: int
    radius: float
    radius_multiplier: float
    falloff: float
    sample_distribution: float
    thin_occluder: float
    final_power: float
    depth_mip_offset: float
    temporal: float


def quality(frame_ms):
    # frame_ms is the measured whole-frame interval for the selected view.
    if frame_ms is None: return 3
    if not np.isfinite(frame_ms) or frame_ms<=0: raise ValueError("invalid present interval")
    if frame_ms < 20.: return 3       # above 50 FPS: Ultra
    if frame_ms <= 25.: return 1      # 40 through 50 FPS: Medium
    if frame_ms < 1000./15: return 0  # above 15 and below 40: Low
    return None                       # 15 FPS or below: skip the AO pass


def propose_ao(picture,history_valid):
    if picture is None or picture.depth_units is None: return None
    f=picture
    tier=quality(f.frame_ms)
    if tier is None: return None
    moving=1. if f.speed is None else unit(f.speed/.60)
    # Convert screen-relative reach into scene units once. Multiplier only trims that reach.
    radius=float(np.clip(2*f.depth_units*(.009+.020*(1-f.thin))/f.focal,.01,100))
    return AO(tier, 1 if f.thin>.52 else 2, 100,
              radius,1.30-.25*f.fog, .54+.33*f.thin, 1.48+1.32*f.thin,
              .06+.86*f.thin, 3.25-.35*f.fog,
              2.9+2.3*f.thin+.5*f.detail,
              (.78-.56*moving) if history_valid and f.speed is not None else 0.)


class SceneReferee:
    """Own continuous transitions; threshold quality and invalid temporal history take effect immediately."""
    def __init__(self):
        self.last_ao=None
        self.denoise_wait=0.
        self.pending_denoise=None

    def apply(self,proposal,dt,reset=False):
        if not np.isfinite(dt) or dt<=0: raise ValueError("invalid frame interval")
        if proposal is None or reset or self.last_ao is None:
            self.last_ao=proposal
            self.denoise_wait=0.
            self.pending_denoise=None
            return proposal
        last=self.last_ao
        k=1-np.exp(-min(dt,.25)/.22)
        values={n:getattr(last,n)+k*(v-getattr(last,n)) for n,v in asdict(proposal).items()}
        # Amount is integral at submission. Keep the fractional accumulator to avoid rounding stalls.
        denoise=last.denoise
        if self.pending_denoise!=proposal.denoise:
            self.denoise_wait=0.
            self.pending_denoise=proposal.denoise
        if denoise!=proposal.denoise:
            self.denoise_wait+=dt
            if self.denoise_wait>=.20: denoise=proposal.denoise; self.denoise_wait=0.
        else: self.denoise_wait=0.
        values.update(quality=proposal.quality,denoise=denoise)
        if proposal.temporal==0: values["temporal"]=0.
        self.last_ao=AO(**values)
        return replace(self.last_ao,amount=round(self.last_ao.amount))


@dataclass(frozen=True)
class Shape:
    reach: float=1.
    dark: float=1.
    lift: float=1.
    wide: float=1.
    edge: float=1.
    haze: float=1.
    guard: float=1.


def propose_shape(picture):
    if picture is None: return Shape()
    p=picture
    return Shape(1.08-.17*p.thin,1.12-.20*p.existing_shadow,1.12-.14*p.fog,
                 1.08-.15*p.thin,1.08-.27*p.thin,.92+.32*p.fog,.92+.28*p.existing_shadow)


class AdaptiveForm(FormReferee):
    def __init__(self):
        super().__init__()
        self.request=Shape()
        self.current=None

    def choose(self,speed,exposure_span,dt,reset,pressure,frame_ms=None):
        c=super().choose(speed,exposure_span,dt,reset,pressure,frame_ms)
        s=self.request
        if self.current is not None and not reset:
            k=1-np.exp(-min(dt,.25)/.22)
            s=Shape(**{n:getattr(self.current,n)+k*(v-getattr(self.current,n))
                       for n,v in asdict(s).items()})
        self.current=s
        return Controls(c.radius*s.reach,c.dark*s.dark,c.light*s.lift,
                        float(np.clip(c.wide*s.wide,.25,.90)),4 if frame_ms is None or frame_ms<=20. else 2,
                        s.edge,s.haze,s.guard)
