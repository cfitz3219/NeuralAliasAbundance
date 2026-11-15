"""Authored surface + bounded, fine-scale learned detail. No procedural noise.

The normal-map combination uses partial derivatives (normal.xy / normal.z),
not RGB averaging. The broad prediction is discarded; original normals and
roughness remain the base. Periodic filtering matches declared wrap materials.
"""
import numpy as np
from .detail import Material, check, unit


VERSION = '2'
SETTINGS = dict(sigma_source_texels=6., normal_detail_gain=3., roughness_detail_gain=2.,
                max_added_slope=.35, max_roughness_delta=.12, protected_slope_threshold=.04,
                method='band-limited learned residual; preserve authored slope direction')


def resize_wrap(a, height, width):
    from .render import sample
    if a.shape[:2] == (height, width):
        return a.copy()
    x, y = np.meshgrid((np.arange(width)+.5)/width, (np.arange(height)+.5)/height)
    scalar = a.ndim == 2
    result = sample(a[...,None] if scalar else a, np.stack([x,y],-1))
    return result[...,0] if scalar else result


def lowpass(a, sigma):
    h,w = a.shape[:2]
    f2 = np.fft.fftfreq(h)[:,None]**2 + np.fft.rfftfreq(w)[None,:]**2
    weight = np.exp(-2*np.pi**2*sigma*sigma*f2)
    if a.ndim == 3:
        weight = weight[...,None]
    return np.fft.irfft2(np.fft.rfft2(a,axes=(0,1))*weight,s=(h,w),axes=(0,1))


def slopes(normal):
    return normal[...,:2]/np.maximum(normal[...,2:3],.05)


def original_at_size(original, colour):
    check(original)
    h,w = colour.shape[:2]
    # Interpolate directions, then normalize. Never interpolate encoded sRGB normals.
    n = unit(resize_wrap(original.normal,h,w))
    r = resize_wrap(original.roughness,h,w)
    return Material(colour.copy(),n,r)


def preserve_authored_direction(detail, base_slopes):
    """Keep the authored groove instead of blanking all model detail near it.

    Remove only the opposing component at a non-flat authored normal. Coherent
    and perpendicular detail survives. This is a geometric constraint, not a
    material classifier; explicit semantic protection still uses the mask.
    """
    squared = np.sum(base_slopes*base_slopes, axis=-1, keepdims=True)
    opposing = np.minimum(0., np.sum(detail*base_slopes, axis=-1, keepdims=True))
    correction = np.where(squared > SETTINGS['protected_slope_threshold']**2,
                          opposing/np.maximum(squared,1e-12), 0.)
    return detail-correction*base_slopes


def combine(original, colour, predicted_normal, predicted_roughness, strength=1., mask=None):
    base = original_at_size(original,colour)
    h,w = base.roughness.shape
    if not np.isfinite(strength) or not 0 <= strength <= 1:
        raise ValueError('surface strength must be in [0,1]')
    if predicted_normal.shape != (h,w,3) or predicted_roughness.shape != (h,w):
        raise ValueError('prediction dimensions do not match colour')
    if not np.isfinite(predicted_normal).all() or not np.isfinite(predicted_roughness).all():
        raise ValueError('non-finite material prediction')
    if np.min(predicted_normal[...,2]) <= 0:
        raise ValueError('prediction must use positive-Z tangent normals')
    if mask is None:
        mask = np.ones((h,w))
    if mask.shape != (h,w) or not np.isfinite(mask).all() or np.min(mask)<0 or np.max(mask)>1:
        raise ValueError('invalid protected-region mask')
    if strength == 0:
        return base
    scale = max(h/original.roughness.shape[0],w/original.roughness.shape[1])
    sigma = SETTINGS['sigma_source_texels']*scale
    pred_slope = slopes(unit(predicted_normal))
    detail = (pred_slope-lowpass(pred_slope,sigma))*SETTINGS['normal_detail_gain']
    base_slopes = slopes(base.normal)
    detail = preserve_authored_direction(detail,base_slopes)
    # A fixed, documented relief range; no per-material RMS normalization, noise
    # injection or automatic gain escalation for weak/flat model predictions.
    length = np.linalg.norm(detail,axis=-1,keepdims=True)
    detail *= np.minimum(1.,SETTINGS['max_added_slope']/np.maximum(length,1e-12))
    detail *= (mask*strength)[...,None]
    xy = base_slopes+detail
    result_normal = unit(np.concatenate([xy,np.ones((h,w,1))],-1))
    rough_detail = (predicted_roughness-lowpass(predicted_roughness,sigma))*SETTINGS['roughness_detail_gain']
    limit = SETTINGS['max_roughness_delta']
    rough_detail = np.clip(rough_detail,-limit,limit)*mask*strength
    result_rough = np.clip(base.roughness+rough_detail,0,1)
    protected = mask == 0
    result_normal[protected] = base.normal[protected]
    result_rough[protected] = base.roughness[protected]
    return Material(base.color,result_normal,result_rough)
