"""Authored surface + bounded fine/medium learned detail. No procedural noise.

The normal-map combination uses partial derivatives (normal.xy / normal.z),
not RGB averaging. The broad prediction is discarded; original normals and
roughness remain the base. Both scales are fixed in material space and baked
into the same maps. Periodic filtering matches declared wrap materials.
"""
import numpy as np
from .detail import Material, check, unit


VERSION = '4'
SETTINGS = dict(sigma_source_texels=6., normal_detail_gain=3., roughness_detail_gain=2.,
                max_added_slope=.35, max_roughness_delta=.12, protected_slope_threshold=.04,
                fine_downsample=2, medium_downsample=4, medium_blur_texels=.75, medium_normal_gain=3.,
                medium_roughness_gain=2.,
                method='fine and medium learned residuals; preserve authored slope direction')


def resize_wrap(a, height, width):
    from .render import sample
    if a.shape[:2] == (height, width):
        return a.copy()
    scalar = a.ndim == 2
    # Large game maps otherwise allocate several full 4K/8K float64 grids and
    # four gathered copies simultaneously. Same texel centres/filter, bounded
    # temporary memory; no change to the approved detail's scale or values.
    source = a[...,None] if scalar else a
    result = np.empty((height,width,source.shape[-1]), dtype=np.float64)
    for start in range(0,height,64):
        end = min(height,start+64)
        x,y = np.meshgrid((np.arange(width)+.5)/width,(np.arange(start,end)+.5)/height)
        result[start:end] = sample(source,np.stack([x,y],-1))
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


def medium_residual(prediction, height, width, sigma):
    # Same material UVs at a coarser inference scale. Reject both pixel-scale
    # noise and broad shape; never change the field with camera distance.
    scale = height/prediction.shape[0]
    band = lowpass(prediction,SETTINGS['medium_blur_texels'])-lowpass(prediction,sigma/scale)
    return resize_wrap(band,height,width)


def combine(original, colour, predicted_normal, predicted_roughness, strength=1., mask=None,
            medium_normal=None, medium_roughness=None, fine_downsample=1):
    base = original_at_size(original,colour)
    h,w = base.roughness.shape
    if not np.isfinite(strength) or not 0 <= strength <= 1:
        raise ValueError('surface strength must be in [0,1]')
    if fine_downsample not in (1,2) or h%fine_downsample or w%fine_downsample:
        raise ValueError('fine inference scale must be 1 or 2 and divide the output')
    fh,fw=h//fine_downsample,w//fine_downsample
    if predicted_normal.shape != (fh,fw,3) or predicted_roughness.shape != (fh,fw):
        raise ValueError('prediction dimensions do not match fine inference scale')
    if not np.isfinite(predicted_normal).all() or not np.isfinite(predicted_roughness).all():
        raise ValueError('non-finite material prediction')
    if np.min(predicted_normal[...,2]) <= 0:
        raise ValueError('prediction must use positive-Z tangent normals')
    if (medium_normal is None) != (medium_roughness is None):
        raise ValueError('medium normal and roughness must be supplied together')
    if medium_normal is not None:
        mh,mw = h//SETTINGS['medium_downsample'],w//SETTINGS['medium_downsample']
        if medium_normal.shape!=(mh,mw,3) or medium_roughness.shape!=(mh,mw):
            raise ValueError('medium prediction dimensions do not match inference scale')
        if not np.isfinite(medium_normal).all() or not np.isfinite(medium_roughness).all() or np.min(medium_normal[...,2])<=0:
            raise ValueError('invalid medium prediction')
    if mask is None:
        mask = np.ones((h,w))
    if mask.shape != (h,w) or not np.isfinite(mask).all() or np.min(mask)<0 or np.max(mask)>1:
        raise ValueError('invalid protected-region mask')
    if strength == 0:
        return base
    scale = max(h/original.roughness.shape[0],w/original.roughness.shape[1])
    sigma = SETTINGS['sigma_source_texels']*scale
    pred_slope = slopes(unit(predicted_normal))
    # Half-size inference doubles the model's texel-scale detail in material
    # space without zooming/cropping the original map or moving its features.
    detail = resize_wrap(pred_slope-lowpass(pred_slope,sigma/fine_downsample),h,w)*SETTINGS['normal_detail_gain']
    if medium_normal is not None:
        detail += medium_residual(slopes(unit(medium_normal)),h,w,sigma)*SETTINGS['medium_normal_gain']
    base_slopes = slopes(base.normal)
    detail = preserve_authored_direction(detail,base_slopes)
    # A fixed, documented relief range; no per-material RMS normalization, noise
    # injection or automatic gain escalation for weak/flat model predictions.
    length = np.linalg.norm(detail,axis=-1,keepdims=True)
    detail *= np.minimum(1.,SETTINGS['max_added_slope']/np.maximum(length,1e-12))
    detail *= (mask*strength)[...,None]
    xy = base_slopes+detail
    result_normal = unit(np.concatenate([xy,np.ones((h,w,1))],-1))
    rough_detail = resize_wrap(predicted_roughness-lowpass(predicted_roughness,sigma/fine_downsample),h,w)*SETTINGS['roughness_detail_gain']
    if medium_roughness is not None:
        rough_detail += medium_residual(medium_roughness,h,w,sigma)*SETTINGS['medium_roughness_gain']
    limit = SETTINGS['max_roughness_delta']
    rough_detail = np.clip(rough_detail,-limit,limit)*mask*strength
    result_rough = np.clip(base.roughness+rough_detail,0,1)
    protected = mask == 0
    result_normal[protected] = base.normal[protected]
    result_rough[protected] = base.roughness[protected]
    return Material(base.color,result_normal,result_rough)
