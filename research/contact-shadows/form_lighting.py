"""Offline geometry-driven lighting study. Outputs independent contact and form layers in stops."""
from dataclasses import dataclass
import numpy as np

from contact_shadow import AXES, LUMA, neighbor


@dataclass(frozen=True)
class Controls:
    radius: float
    dark: float
    light: float
    wide: float
    directions: int
    edge_scale: float=1.
    haze_scale: float=1.
    guard_scale: float=1.


@dataclass(frozen=True)
class Budget:
    mean: float = .24
    tile: float = .82
    contrast_min: float = .92
    contrast_max: float = 1.10


def box(values, radius):
    """Symmetric box average, with replicated image borders."""
    r = max(1, int(radius))
    pad = np.pad(values, r, mode="edge")
    summed = np.pad(pad, ((1, 0), (1, 0))).cumsum(0, dtype=np.float64).cumsum(1)
    k = 2*r+1
    return ((summed[k:,k:] - summed[:-k,k:] - summed[k:,:-k] + summed[:-k,:-k]) / (k*k)).astype("f4")


def tile_means(magnitude, valid):
    h, w = valid.shape
    side = max(1, round(h/16))
    pad = ((0,(-h)%side), (0,(-w)%side))
    def sums(a):
        a = np.pad(a, pad)
        return a.reshape(a.shape[0]//side, side, -1, side).sum(axis=(1,3), dtype=np.float64)
    return sums(magnitude)/np.maximum(1, sums(valid)), side


def broad(lum):
    low, high = np.quantile(lum, [.1,.9])
    return max(1e-6, np.log2(max(high, 1e-6)/max(low, 1e-6)))


class FormReferee:
    def __init__(self):
        self.settings = None

    def choose(self, speed, exposure_span, dt, reset, pressure, frame_ms=None):
        moving = 1. if speed is None else float(np.clip(speed/.60, 0, 1))
        soft = float(np.clip((5-exposure_span)/4, 0, 1))
        target = np.array([.055+.038*soft-.020*moving, 1.65+.55*soft-.18*moving,
                           1.3+.55*soft-.14*moving, .61+.17*soft-.10*moving])
        target *= np.array([1-.23*pressure,1.,1.,1-.3*pressure])
        if reset or self.settings is None:
            self.settings = target
        else:
            self.settings += (1-np.exp(-dt/.24))*(target-self.settings)
        return Controls(*self.settings, 2 if pressure>.72 else 4)

    def layers(self, rgb, z, reversed_depth, motion=None, contact_dial=1., form_dial=1.,
               budget=Budget(), dt=1/60, reset=False, gpu_ms=None, allowance_ms=1.5, frame_ms=None):
        if z.ndim != 2 or rgb.shape != (*z.shape, 3):
            raise ValueError("Color and depth must be registered at matching dimensions")
        zeros = np.zeros_like(z, dtype="f4")
        def off(reason):
            self.settings = None
            return zeros.copy(), zeros.copy(), dict(state=reason)
        if (reversed_depth not in (True, False) or min(z.shape)<16 or
            not np.isfinite([contact_dial, form_dial, dt]).all() or dt<=0):
            return off("no reliable input")
        if not 0 <= min(contact_dial, form_dial) <= max(contact_dial, form_dial) <= 1:
            raise ValueError("Strength must be in [0, 1]")
        if (not np.isfinite(allowance_ms) or allowance_ms<=0 or
            any(t is not None and (not np.isfinite(t) or t<0) for t in (gpu_ms,frame_ms))):
            return off("no reliable timing input")
        if (gpu_ms is not None and gpu_ms>=allowance_ms) or (frame_ms is not None and frame_ms>=25):
            return off("time allowance")
        pressure = max(0. if gpu_ms is None else np.clip((gpu_ms/allowance_ms-.65)/.35,0,1),
                       0. if frame_ms is None else np.clip((frame_ms-20)/5,0,1))
        if contact_dial == form_dial == 0:
            return off("off")
        q = z if reversed_depth else 1-z
        valid = (q>0)&(q<1)&np.isfinite(q)&np.isfinite(rgb).all(axis=2)&(rgb.min(axis=2)>=0)
        if valid.sum()<256:
            return off("no reliable scene input")
        q = np.where(valid, q, 0).astype("f4")
        lo, hi = np.quantile(q[valid], [.03,.97])
        if hi-lo<2e-6:
            return off("no reliable depth variation")
        lum = np.where(valid, rgb@LUMA, 0)
        original = lum[valid]
        before = broad(original)
        speed = None
        if motion is not None:
            if motion.shape != (*q.shape, 2): return off("unregistered motion")
            h,w=q.shape
            yy,xx=np.ogrid[:h,:w]
            mv = np.isfinite(motion).all(axis=2)&valid
            mv &= (np.abs(motion).max(axis=2)<.25*max(h,w))
            mv &= (xx+motion[...,0]>=0)&(xx+motion[...,0]<w)&(yy+motion[...,1]>=0)&(yy+motion[...,1]<h)
            if mv.sum()>=max(256,valid.sum()*.5):
                speed = float(np.percentile(np.linalg.norm(motion[mv],axis=1),75)/(h*dt))
        control = self.choose(speed, before, dt, reset, pressure, frame_ms)
        h,w=q.shape
        light = np.log2(np.maximum(lum, 1e-6))
        base = box(light*valid,max(2,h*.009))/np.maximum(box(valid.astype("f4"),max(2,h*.009)),1e-6)
        surround = box(light*valid,max(4,h*.043))/np.maximum(box(valid.astype("f4"),max(4,h*.043)),1e-6)
        observed_shadow = np.maximum(0, surround-base)
        guard = np.exp2(-1.35*control.guard_scale*observed_shadow)
        # Samples are in directed device depth. Opposite differences cancel any plane's affine slope.
        # The local derivative test vetoes immediate silhouettes; far, isolated steps cannot vote for a fold.
        unit = np.maximum(q, (hi-lo)*.005)
        edge = zeros.copy()
        for dx,dy in ((1,0),(0,1),(-1,0),(0,-1)):
            edge = np.maximum(edge, np.abs(q-neighbor(q,dx,dy)))
        edge_ok = np.clip(1-(edge/unit-.16*control.edge_scale)/(.38*control.edge_scale), 0, 1)*valid
        wide_support = zeros.copy()
        narrow_support = zeros.copy()
        for band, weight in ((.23,1-control.wide), (1.,control.wide)):
            total = zeros.copy()
            vote = zeros.copy()
            for dx,dy in AXES[:control.directions]:
                r = max(2,int(round(control.radius*h*band/np.hypot(dx,dy))))
                a = neighbor(q,dx*r,dy*r)-q
                b = neighbor(q,-dx*r,-dy*r)-q
                k = max(1,r//2)
                ma = neighbor(q,dx*k,dy*k)-q
                mb = neighbor(q,-dx*k,-dy*k)-q
                # Reject abrupt one-sided steps, where interpolating through depth crosses an outline.
                tol = .009*unit + (hi-lo)*.00025
                def continuity(end, mid):
                    err = np.abs(mid - end*(k/r))
                    abrupt = ((np.abs(end)>5e-7) &
                              (np.minimum(np.abs(mid),np.abs(end-mid))<.025*np.abs(end)))
                    return np.where(abrupt,0,np.clip(1.35-err/(.48*np.abs(end)+tol),0,1))
                coherent = np.minimum(continuity(a,ma),continuity(b,mb))
                mask = valid.copy()
                for n in (-r,-k,k,r):
                    mask &= neighbor(valid, dx*n, dy*n)
                if dx: mask[:,:r]=False; mask[:,w-r:]=False
                if dy: mask[:r]=False; mask[h-r:]=False
                coherent *= mask
                # Screen scale adapts to depth precision; both inward and outward responses saturate.
                raw = -(a+b)
                curvature = np.sign(raw)*np.maximum(np.abs(raw)-5e-7,0)
                bend = np.tanh(curvature/(unit*(.009+band*.024)+(hi-lo)*.00012))
                total += coherent*bend
                vote += coherent
            field = edge_ok*total/np.maximum(1.8,vote)
            wide_support += weight*field
            if band < 1:
                narrow_support = field
        # Shape authorizes lighting on a surface. Existing low-frequency shading
        # reinforces its direction; a flat painted pattern cannot authorize an effect.
        # A veil of fog flattens local lighting in the far field. Preserve that cue.
        # Use a relative depth rank and the scene's local spread in stops, without guessing distance.
        far_start, near_end = np.quantile(q[valid], [.14,.53])
        proximity = np.clip((q-far_start)/max(near_end-far_start,1e-7),0,1)
        spread = box(np.abs(light-surround)*valid, max(2,h*.025))/np.maximum(box(valid.astype("f4"),max(2,h*.025)),1e-6)
        atmosphere = .06+.94*np.maximum(proximity,np.clip((spread-.19)/.85,0,1))
        atmosphere = atmosphere**control.haze_scale
        wide_support *= atmosphere
        narrow_support *= atmosphere
        gate = np.minimum(1, np.abs(wide_support)*3.4)
        painted = np.tanh((base-surround)/.65)
        form = np.clip(wide_support*1.9 + .68*painted*gate, -1,1)
        recess = np.clip(np.maximum(-wide_support*1.7, -narrow_support*.72),0,1)
        contact = -contact_dial*control.dark*recess*guard
        form = form_dial*np.where(form>0, control.light*form, control.dark*form*guard)
        # Curvature and contact may nominate the same recess. Keep the larger darkening.
        shade = np.maximum(-contact,np.maximum(-form,0))
        lift = np.maximum(form,0)
        permit_dark = np.maximum(0,np.log2(np.maximum(lum,1e-8)/.005))
        shade = np.minimum(shade,permit_dark)*valid
        lift *= valid & (lum>=.005)
        local, side = tile_means(shade,valid)
        # Interpolate conservative local limits at tile boundaries.
        scales = np.minimum(1,budget.tile/np.maximum(local,1e-8))
        # Every interpolated scale stays below the cap of the tile containing the pixel.
        # Taking the minimum across each corner's four touching tiles prevents seams.
        grid = np.pad(scales, 1, mode="edge")
        corners = np.minimum.reduce([grid[:-1,:-1],grid[:-1,1:],grid[1:,:-1],grid[1:,1:]])
        rows,cols = np.indices((h,w))
        ty,tx = (rows+.5)/side,(cols+.5)/side
        iy,ix = ty.astype(int),tx.astype(int)
        fy,fx = ty-iy,tx-ix
        regional = ((1-fy)*((1-fx)*corners[iy,ix]+fx*corners[iy,ix+1])
                    +fy*((1-fx)*corners[iy+1,ix]+fx*corners[iy+1,ix+1]))
        shade *= regional
        shade *= min(1, budget.mean/max(float(shade[valid].mean()),1e-8))
        contact_share = (-contact>=np.maximum(-form,0)) & (contact<0)
        contact = np.where(contact_share,-shade,0).astype("f4")
        form = (lift-np.where(contact_share,0,shade)).astype("f4")
        delta = contact+form
        crossing=(lum>=.005)&(lum*np.exp2(delta)<.005)
        contact[crossing]=0; form[crossing]=0
        delta = contact+form
        factor = 1.
        def allowed(scale):
            ratio = broad(original*np.exp2(delta[valid]*scale))/before
            return budget.contrast_min <= ratio <= budget.contrast_max
        if not allowed(1.):
            low,high = 0.,1.
            for _ in range(10):
                mid = (low+high)*.5
                if allowed(mid): low = mid
                else: high = mid
            factor = low
        contact *= factor; form *= factor
        combined = contact+form
        stats=measures(combined, lum, valid, before)
        stats.update(state="measured" if np.any(combined) else "no reliable response",
                     speed=speed, gpu_ms=gpu_ms, controls=vars(control), contrast_scale=factor,
                     contact_mean=float(-contact[valid].mean()),form_abs_mean=float(np.abs(form[valid]).mean()))
        return contact, form, stats


def measures(delta, lum, valid, before):
    changes = delta[valid]
    after = lum[valid]*np.exp2(changes)
    dark = np.maximum(-delta,0)
    tiles,_ = tile_means(dark,valid)
    return dict(covered_pixels=int(valid.sum()),mean_dark=float(dark[valid].mean()),
                tile_dark=float(tiles.max()), contrast=float(broad(after)/before),
                near_black_before=float((lum[valid]<.005).mean()),near_black_after=float((after<.005).mean()),
                mean_absolute=float(np.abs(changes).mean()),share_quarter_stop=float((np.abs(changes)>=.25).mean()),
                share_half_stop=float((np.abs(changes)>=.5).mean()),mean_lift=float(np.maximum(changes,0).mean()))
