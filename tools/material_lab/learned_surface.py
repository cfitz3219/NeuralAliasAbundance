"""Authored surface + bounded, fine-scale learned detail. No procedural noise.

The normal-map combination uses partial derivatives (normal.xy / normal.z),
not RGB averaging. The broad prediction is discarded; original normals and
roughness remain the base. Periodic filtering matches declared wrap materials.
"""
import numpy as np
from .detail import Material, check, gradient, unit


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
    # Retain authored seams, material boundaries and sharp painted markings.
    nx,ny = gradient(original.normal)
    rx,ry = gradient(original.roughness)
    cx,cy = gradient(original.color[...,:3])
    edges = (np.sum(nx*nx+ny*ny,-1)/.05**2 + (rx*rx+ry*ry)/.08**2
             + np.sum(cx*cx+cy*cy,-1)/.08**2)
    protection = resize_wrap(1/(1+edges*edges),h,w)*mask
    scale = max(h/original.roughness.shape[0],w/original.roughness.shape[1])
    sigma = 1.5*scale
    pred_slope = slopes(unit(predicted_normal))
    detail = pred_slope-lowpass(pred_slope,sigma)
    # Limits bound the addition; weak predictions are never amplified to a target RMS.
    length = np.linalg.norm(detail,axis=-1,keepdims=True)
    detail *= np.minimum(1.,.22/np.maximum(length,1e-12))
    detail *= (protection*strength)[...,None]
    xy = slopes(base.normal)+detail
    result_normal = unit(np.concatenate([xy,np.ones((h,w,1))],-1))
    rough_detail = predicted_roughness-lowpass(predicted_roughness,sigma)
    rough_detail = np.clip(rough_detail,-.12,.12)*protection*strength
    result_rough = np.clip(base.roughness+rough_detail,0,1)
    protected = mask == 0
    result_normal[protected] = base.normal[protected]
    result_rough[protected] = base.roughness[protected]
    return Material(base.color,result_normal,result_rough)
