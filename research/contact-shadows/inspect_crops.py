"""Render matched crops and an explicit stop scale from the selected saved layers."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from study import read_frame, LUMA, display


def inspect(folder, out):
    info = json.loads((out/"metrics.json").read_text())
    _, rgb, _, _ = read_frame(folder,info["selected_frame"],info["step"])
    with np.load(out/"selected-layers.npz") as saved:
        delta = saved["delta_stops"]
        intel = saved["intel_budgeted_stops"] if "intel_budgeted_stops" in saved else None
    h,w = delta.shape
    side = min(80,h,w)
    # Find the strongest complete square, independently of the original picture's color.
    integral = np.pad((-delta).astype(np.float64).cumsum(0).cumsum(1), ((1,0),(1,0)))
    sums = integral[side:,side:]-integral[:-side,side:]-integral[side:,:-side]+integral[:-side,:-side]
    y,x = np.unravel_index(np.argmax(sums),sums.shape)
    area = np.s_[y:y+side,x:x+side]
    lum = rgb@LUMA
    exposure = .30/max(float(np.median(lum[lum>1e-5])),1e-4)
    try:
        regular = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",17)
        bold = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf",22)
    except OSError:
        regular=bold=ImageFont.load_default()
    panels = [("Input",display(rgb[area],exposure))]
    if intel is not None:
        panels.append(("Intel XeGTAO",display(rgb[area]*np.exp2(intel[area][...,None]),exposure)))
    panels.append(("Referee contact shadows",display(rgb[area]*np.exp2(delta[area][...,None]),exposure)))
    amount = np.clip(-delta[area]/.10,0,1)
    heat = np.stack((amount,amount*.77,amount*.31),axis=2)
    panels.append(("Where our pass darkened",Image.fromarray(np.uint8(255*heat))))
    k=250
    canvas=Image.new("RGB",(k*len(panels),370),"#151c24")
    draw=ImageDraw.Draw(canvas)
    draw.text((9,4),f"Strongest {side} × {side} crop   /   frame {info['selected_frame']}",font=bold,fill="white")
    for i,(label,picture) in enumerate(panels):
        draw.text((k*i+6,41),label,font=regular,fill="white")
        canvas.paste(picture.resize((k-4,k-4),Image.Resampling.NEAREST),(k*i+2,70))
    draw.text((7,324),"Shaded pictures use actual strength. In the last panel, black = 0 and gold = 0.10 stops or more.",font=regular,fill="#c7d3df")
    canvas.save(out/"inspection.png")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture",type=Path)
    parser.add_argument("output",type=Path)
    a=parser.parse_args()
    inspect(a.capture,a.output)
