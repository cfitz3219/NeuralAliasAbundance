"""Controlled comparison and a reproducible camera rotation with known displacement."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from contact_shadow import Referee
from test_contact_shadow import scene


def controlled_figure(out, rgb, depth, dist, shaded):
    still = np.zeros((*depth.shape, 2), np.float32)
    delta, _, _ = Referee().evaluate(rgb, depth, True, still)
    stripe = (dist == 0) & (np.indices(depth.shape)[0] > 12) & (np.indices(depth.shape)[0] < 132)
    measurements = [float(-delta[stripe & mask].mean()) for mask in (~shaded, shaded)]
    canvas = Image.new("RGB", (850, 420), "#151c24")
    d = ImageDraw.Draw(canvas)
    try:
        title = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf", 28)
        body = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 19)
    except OSError:
        title = body = ImageFont.load_default()
    d.text((24, 15), "Two identical grooves, different existing light", font=title, fill="white")
    d.text((24, 58), "Controlled scene  /  same exposure in every crop", font=body, fill="#bfcbd5")
    d.text((188, 106), "Washed-out groove", font=body, fill="white")
    d.text((521, 106), "Already shaded groove", font=body, fill="white")
    out_rgb = rgb * np.exp2(delta[..., None])
    for i, (label, arr) in enumerate((("Input", rgb), ("With contact pass", out_rgb))):
        d.text((20, 164 + i * 82), label, font=body, fill="#ecf0f4")
        for j, cx in enumerate((132, 156)):
            crop = arr[62:72, cx-9:cx+10]
            srgb = 1.055 * crop**(1/2.4) - .055
            img = Image.fromarray(np.uint8(np.clip(srgb, 0, 1)*255)).resize((300, 65), Image.Resampling.NEAREST)
            canvas.paste(img, (188+j*333, 143+i*82))
    d.text((20, 313), "Mean added at center", font=body, fill="#bfcbd5")
    for j, v in enumerate(measurements):
        label = "< 0.000001 stops" if v < .000001 else f"{v:.3f} stops"
        d.text((215+j*333, 309), label, font=title, fill="#9cddd2")
    d.text((20, 373), "Synthetic linear input. Crops show actual shading strength.", font=body, fill="#bfcbd5")
    canvas.save(out/"same-geometry.png")
    return measurements


def warp_coords(shape, angle):
    h, w = shape
    yy, xx = np.indices(shape, dtype=np.float32)
    f = w*.8
    u, v = (xx-(w-1)*.5)/f, (yy-(h-1)*.5)/f
    c, s = np.cos(angle), np.sin(angle)
    plane_z = c - s*u
    return (f*(c*u+s)/plane_z+(w-1)*.5, f*v/plane_z+(h-1)*.5, plane_z)


def sample(arr, x, y):
    h, w = arr.shape[:2]
    x = np.clip(x, 0, w-1)
    y = np.clip(y, 0, h-1)
    i, j = x.astype(int), y.astype(int)
    tx, ty = x-i, y-j
    if arr.ndim==3:
        tx, ty = tx[...,None], ty[...,None]
    right, below = np.minimum(i+1,w-1), np.minimum(j+1,h-1)
    return ((1-ty)*((1-tx)*arr[j,i]+tx*arr[j,right]) +
            ty*((1-tx)*arr[below,i]+tx*arr[below,right])).astype(np.float32)


class FrozenReferee(Referee):
    def choose(self, *args):
        return self.settings or super().choose(*args)


def rotation(rgb, depth):
    auto, frozen = Referee(), FrozenReferee()
    pos = 0.
    h, w = depth.shape
    yy, xx = np.indices(depth.shape)
    results = []
    for i in range(45):
        turn = .00275 if 10<=i<30 else 0.
        pos += turn
        x, y, z = warp_coords(depth.shape, pos)
        valid = (x>=0)&(x<w-1)&(y>=0)&(y<h-1)
        color = sample(rgb,x,y)
        q = np.where(valid,sample(depth,x,y)*z,0).astype(np.float32)
        px, py, _ = warp_coords(depth.shape,turn)
        mv = np.stack((px-xx,py-yy),axis=2).astype(np.float32)
        d,_,s = auto.evaluate(color,q,True,mv,adjacent=i>0)
        f,_,fixed = frozen.evaluate(color,q,True,mv,adjacent=i>0)
        results.append(dict(frame=i, speed=s["motion_heights_per_second"],
                            reach=s["settings"]["reach"],history=s["settings"]["history"],
                            frozen_reach=fixed["settings"]["reach"],frozen_history=fixed["settings"]["history"],
                            layer_difference=float(np.abs(f-d)[valid].mean()),
                            worst_tile=s["worst_tile_stops"],mean=s["mean_stops"]))
    assert results[29]["reach"] < results[9]["reach"]
    assert results[44]["reach"] > results[29]["reach"]
    assert results[29]["frozen_reach"] == results[9]["frozen_reach"]
    assert results[29]["layer_difference"] > 0
    return results


if __name__=="__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    out = parser.parse_args().output
    out.mkdir(exist_ok=True,parents=True)
    rgb,depth,dist,shaded = scene()
    grooves = controlled_figure(out,rgb,depth,dist,shaded)
    frames = rotation(rgb,depth)
    (out/"controlled.json").write_text(json.dumps(dict(groove_stops=grooves,rotation_frames=frames),indent=2))
