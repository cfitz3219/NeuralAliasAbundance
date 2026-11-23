"""Create a self-contained full-frame viewer and a smaller two-frame flipbook."""
import argparse
import base64
import json
import numpy as np
from io import BytesIO
from html import escape
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

MODES = ("original","contact","form","combined")
LABELS = ("Original","Contact shadows","Curved lighting","Both layers")


def static_proof(folder):
    with Image.open(folder/"original.png") as picture:
        original = np.asarray(picture.convert("RGB"),dtype=np.int16)
    with Image.open(folder/"combined.png") as picture:
        combined = np.asarray(picture.convert("RGB"))
    code_difference = np.abs((combined-original)@np.array([.2126,.7152,.0722],dtype="f4"))
    stats = {"whole_frame_share_at_least_8_luma_codes":float((code_difference>=8).mean())}
    (folder/"display.json").write_text(json.dumps(stats,indent=2))
    height,width=original.shape[:2]
    crop=(width//4,height//4,3*width//4,3*height//4)
    w=900
    h=round(w*(crop[3]-crop[1])/(crop[2]-crop[0]))
    canvas=Image.new("RGB",(w,2*(h+33)),"#121820")
    draw=ImageDraw.Draw(canvas)
    try: font=ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf",19)
    except OSError: font=ImageFont.load_default()
    for i,(label,values) in enumerate((("Original",original),("Contact shadows + curved lighting",combined))):
        photo=Image.fromarray(values.astype("uint8")).crop(crop).resize((w,h),Image.Resampling.LANCZOS)
        draw.text((12,i*(h+33)+4),label,font=font,fill="white")
        canvas.paste(photo,(0,i*(h+33)+33))
    canvas.save(folder/"comparison.jpg",quality=94,subsampling=0)


def create(folder, title):
    static_proof(folder)
    blobs=[]
    frames=[]
    for mode in MODES:
        with Image.open(folder/(mode+".png")) as im:
            img=im.convert("RGB")
            encoded=BytesIO()
            img.save(encoded,"JPEG",quality=94,subsampling=0,optimize=True)
            blobs.append("data:image/jpeg;base64,"+base64.b64encode(encoded.getvalue()).decode("ascii"))
            if mode in ("original","combined"):
                img.thumbnail((1350,630),Image.Resampling.LANCZOS)
                framed=Image.new("RGB",(img.width,img.height+45),"#192128")
                framed.paste(img,(0,45));draw=ImageDraw.Draw(framed)
                try: font=ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf",25)
                except OSError: font=ImageFont.load_default()
                draw.text((16,5),title+"    /    "+("Original" if mode=="original" else "Contact shadows + curved lighting"),fill="white",font=font)
                frames.append(framed)
    frames[0].save(folder/"before-after.webp",save_all=True,append_images=frames[1:],duration=1800,loop=0,quality=90,method=5)
    # Loading every image in the same fixed space makes switching immediate.
    pictures="".join(f'<img {"" if i==3 else "hidden"} alt="{label} — {escape(title)}" src="{src}">' for i,(label,src) in enumerate(zip(LABELS,blobs)))
    buttons="".join(f'<button aria-pressed="{str(i==3).lower()}" data-mode="{i}">{label}</button>' for i,label in enumerate(LABELS))
    page = """<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TITLE — Lighting comparison</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#151a20;color:#f3f5f6;font:16px/1.4 system-ui,sans-serif}
main{max-width:1800px;margin:auto;padding:18px clamp(10px,2.2vw,32px)}header{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap}
h1{font-size:clamp(20px,3vw,30px);margin:0}nav{display:flex;flex-wrap:wrap;gap:7px}button{font:inherit;background:#252e39;color:#f4f5f6;padding:7px 14px;border:1px solid #516070;border-radius:5px;cursor:pointer}
button[aria-pressed=true]{background:#b1e7d2;color:#14251e;border-color:#b1e7d2}button:focus-visible{outline:3px solid white;outline-offset:2px}
#frame{overflow:auto;line-height:0;margin:15px 0;background:#0c1015;touch-action:pan-x pan-y}img{width:100%;height:auto}img[hidden]{display:none}
#frame.native img{width:auto;max-width:none}footer{display:flex;align-items:center;gap:16px;flex-wrap:wrap;color:#ccd4db}p{margin:0}
</style><main><header><h1>TITLE</h1><nav aria-label="Lighting layers">BUTTONS</nav></header>
<div id="frame">PICTURES</div><footer><button id="zoom" aria-pressed="false">View at 100%</button>
<p>Hold Space or hold the picture to see the original. Keys 1–4 select a layer. All views share one exposure.</p></footer></main>
<script>
const frame=document.getElementById('frame'),pictures=Array.from(frame.children),buttons=Array.from(document.querySelectorAll('[data-mode]'));
let chosen=3,holding=false;
function show(){pictures.forEach((image,i)=>image.hidden=i!==(holding?0:chosen));buttons.forEach((b,i)=>b.setAttribute('aria-pressed',String(i===(holding?0:chosen))))}
buttons.forEach((b,i)=>b.onclick=()=>{chosen=i;holding=false;show()});
document.getElementById('zoom').onclick=e=>{const on=frame.classList.toggle('native');e.currentTarget.setAttribute('aria-pressed',String(on));e.currentTarget.textContent=on?'Fit to window':'View at 100%'};
window.addEventListener('keydown',e=>{if(e.code==='Space'){e.preventDefault();holding=true;show()}else if(/^[1-4]$/.test(e.key)){chosen=Number(e.key)-1;holding=false;show()}});
function release(){holding=false;show()}
window.addEventListener('keyup',e=>{if(e.code==='Space')release()});window.addEventListener('blur',release);
frame.onpointerdown=e=>{if(e.button===0){holding=true;show();frame.setPointerCapture(e.pointerId)}};
frame.onpointerup=frame.onpointercancel=frame.onlostpointercapture=release;
</script></html>"""
    (folder/"compare.html").write_text(page.replace("TITLE",escape(title)).replace("BUTTONS",buttons).replace("PICTURES",pictures),encoding="utf-8")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder",type=Path)
    parser.add_argument("title")
    args=parser.parse_args()
    create(args.folder,args.title)
