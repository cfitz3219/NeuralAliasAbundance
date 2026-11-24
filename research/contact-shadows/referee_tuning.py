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


# Small proposals do not justify a visible adjustment. The relative part also
# handles changes in scene units. Integer settings need their own resolution.
AO_DEADBAND = {
    "amount": 1.0, "radius": .0001, "radius_multiplier": .015, "falloff": .01,
    "sample_distribution": .03, "thin_occluder": .015, "final_power": .04,
    "depth_mip_offset": .06, "denoise": 0., "temporal": .015,
}


def scene_changed(reference, current):
    """Compare only pre-AO evidence. Quality's own cost cannot unlock a visual dial."""
    if reference is None or current is None:
        return True
    for name in ("thin", "detail", "fog", "existing_shadow", "coverage"):
        if abs(getattr(reference, name) - getattr(current, name)) >= .08:
            return True
    for name in ("depth_units", "focal"):
        a, b = getattr(reference, name), getattr(current, name)
        if a != b and (a is None or b is None or abs(a - b) >= .1 * abs(a)):
            return True
    a, b = reference.speed, current.speed
    return a != b and (a is None or b is None or abs(a - b) >= .15)


class VisualTarget:
    """Confirm a direction, and remember a round trip on an equivalent source picture."""
    def __init__(self, value):
        self.value = value
        self.previous = None
        self.changed_at = None
        self.hold_at = None
        self.direction = 0
        self.elapsed = 0.

    def select(self, name, request, dt, picture):
        if self.hold_at is not None:
            if not scene_changed(self.hold_at, picture):
                return self.value
            self.hold_at = None
            self.previous = None
        tolerance = max(AO_DEADBAND[name], .02 * abs(self.value)) if name != "denoise" else 0.
        error = request - self.value
        direction = 0 if abs(error) <= tolerance else (1 if error > 0 else -1)
        if direction == 0 or direction != self.direction:
            self.elapsed = 0.
        self.direction = direction
        if direction:
            self.elapsed += min(dt, .1)
        if self.elapsed >= .20:
            if (self.previous is not None and abs(request - self.previous) <= tolerance
                    and not scene_changed(self.changed_at, picture)):
                self.hold_at = picture
            self.previous, self.value = self.value, request
            self.changed_at = picture
            self.direction, self.elapsed = 0, 0.
        return self.value


class QualityTarget:
    """Downshift immediately. Qualify a recovery and remember the headroom of failed upgrades."""
    def __init__(self):
        self.tier = None
        self.pending = None
        self.elapsed = 0.
        self.total_ms = 0.
        self.promoted_from = {}
        self.failed_ms = {}
        self.reason = "initial"

    def select(self, requested, frame_ms, dt):
        self.reason = "steady"
        if self.tier is None or requested < self.tier:
            self.reason = "initial" if self.tier is None else "performance_drop"
            for tier in list(self.promoted_from):
                if tier > requested:
                    self.failed_ms[tier] = self.promoted_from.pop(tier)
            self.tier = requested
        if requested == self.tier or frame_ms is None:
            self.pending = None
            self.elapsed = self.total_ms = 0.
            return self.tier
        # A previous upgrade to a tier failed at this frame time. A higher
        # tier needs fresh headroom; intermediate qualities can still recover.
        eligible = [tier for tier in (0, 1, 3) if self.tier < tier <= requested
                    and (tier not in self.failed_ms or frame_ms <= self.failed_ms[tier] - .5)]
        if not eligible:
            self.reason = "hold_after_failed_upgrade"
            self.pending = None
            self.elapsed = self.total_ms = 0.
            return self.tier
        self.reason = "confirming_recovery"
        requested = max(eligible)
        if requested != self.pending:
            self.pending = requested
            self.elapsed = self.total_ms = 0.
        sample = min(dt, .1)
        self.elapsed += sample
        self.total_ms += frame_ms * sample
        if self.elapsed >= 1.:
            self.reason = "recovered"
            self.promoted_from[requested] = self.total_ms / self.elapsed
            self.failed_ms.pop(requested, None)
            self.tier = requested
            self.pending = None
            self.elapsed = self.total_ms = 0.
        return self.tier


class SceneReferee:
    """Use separate stability rules for visual targets, performance and history validity."""
    def __init__(self):
        self.last_ao = None
        self.targets = {}
        self.quality_target = QualityTarget()

    def apply(self, proposal, dt, reset=False, *, picture=None):
        if not np.isfinite(dt) or dt <= 0:
            raise ValueError("invalid frame interval")
        frame_ms = picture.frame_ms if picture is not None else dt * 1000
        missing = proposal is None and (picture is None or picture.depth_units is None
                                        or quality(frame_ms) is not None)
        if reset or missing:
            self.__init__()
        if missing:
            return None

        requested = -1 if proposal is None else proposal.quality
        tier = self.quality_target.select(requested, frame_ms, dt)
        if tier < 0:
            self.last_ao = None
            self.targets.clear()
            return None
        if self.last_ao is None:
            self.targets = {name: VisualTarget(getattr(proposal, name)) for name in AO_DEADBAND}
            self.last_ao = replace(proposal, quality=tier)
            return self.last_ao

        values = {"quality": tier}
        blend = 1 - np.exp(-min(dt, .25) / .22)
        for name, target in self.targets.items():
            requested_value = getattr(proposal, name)
            if name in ("temporal", "amount") and requested_value == 0:
                self.targets[name] = VisualTarget(0.)
                values[name] = 0.
                continue
            goal = target.select(name, requested_value, dt, picture)
            last_value = getattr(self.last_ao, name)
            values[name] = goal if name == "denoise" else last_value + blend * (goal - last_value)
        self.last_ao = AO(**values)
        return replace(self.last_ao, amount=round(self.last_ao.amount))


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
