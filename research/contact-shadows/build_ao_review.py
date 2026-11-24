"""Build a portable, blind XeGTAO comparison from two qualified stills and a recorded pan."""
import argparse
import base64
from dataclasses import asdict, replace
import hashlib
from io import BytesIO
import json
from pathlib import Path
import random
from tempfile import TemporaryDirectory

import numpy as np
from projection_trial import blind_matrix
from referee_tuning import propose_ao, observe
from study import display
from tune_replay import SavedFrame, spatial_gpu


def intel_tuning(base):
    """Intel heuristics, matched base radius and quality; no NeuralAlias temporal accumulation."""
    return replace(base, amount=100, radius_multiplier=1.457, falloff=.615, sample_distribution=2.,
                   thin_occluder=0., denoise=1, final_power=2.2, depth_mip_offset=3.3, temporal=0.)


def build(stills, pan, lens, fixture, output, step=2):
    images, trials = [], []
    rng = random.Random(15261)
    def encode(rgb, exposure):
        stream = BytesIO()
        display(rgb, exposure).save(stream, format="WEBP", lossless=True, method=1)
        images.append("data:image/webp;base64," + base64.b64encode(stream.getvalue()).decode())
        return len(images)-1

    def add(title, question, sources, intel, variants, mappings, frame_ids):
        order = [0,1]
        rng.shuffle(order)
        trials.append(dict(title=title, question=question, frames=frame_ids, original=sources, intel=intel,
                           variants=[variants[i] for i in order], settings=[mappings[i] for i in order]))

    # Values are deliberate brackets for a human audition, never production defaults.
    cases = [
        ("Contact strength", "Which keeps objects grounded and still lets you see into the shadows?",
         {"amount":55},{"amount":100}),
        ("Shadow reach", "Which makes contact believable without a broad dark outline?",
         {"radius_multiplier":.8},{"radius_multiplier":1.8}),
        ("Shadow falloff", "Which fades more naturally as the gap between surfaces opens?",
         {"falloff":.32},{"falloff":.9}),
        ("Thin foreground", "Which leaves clean silhouettes while retaining contacts beneath them?",
         {"thin_occluder":0.},{"thin_occluder":.7}),
        ("Fine geometry", "Which preserves small creases without making the terrain look gritty?",
         {"sample_distribution":1.2},{"sample_distribution":2.7}),
        ("Depth mip offset", "Which keeps depth edges clean without dropping thin detail?",
         {"depth_mip_offset":2.4},{"depth_mip_offset":4.8}),
        ("Shadow depth", "Which gives creases shape without making them look painted black?",
         {"final_power":1.6},{"final_power":3.8}),
    ]
    with TemporaryDirectory() as scratch:
        for scene, (folder, frame_index) in enumerate(stills):
            f = SavedFrame(folder, frame_index, step, 16.)
            base = propose_ao(f.picture,False)
            if base is None: raise ValueError("The still needs a qualified projection")
            exposure = .3/max(float(np.median(f.y[f.valid])),1e-4)
            original = [encode(f.rgb,exposure)]
            intel=[encode(f.rgb*np.exp2(spatial_gpu(f,intel_tuning(base),fixture,scratch)[:,:,None]),exposure)]
            for title, question, left, right in cases[scene::2]:
                options = [replace(base,**change) for change in (left,right)]
                variants = []
                for opt in options:
                    delta = spatial_gpu(f,opt,fixture,scratch)
                    variants.append([encode(f.rgb*np.exp2(delta[:,:,None]),exposure)])
                add(title,question,original,intel,variants,[asdict(o) for o in options],
                    [f"{folder.name}/{frame_index}"])
        # Seed each temporal evaluation on the preceding submitted picture. This
        # tests short motion only; it cannot assess accumulated long-history trails.
        previous, previous_options, exposure = None, [], None
        originals, intel, ids = [], [], []
        variants, settings = [[],[],[]], [[],[],[]]
        for idx in range(9):
            f=SavedFrame(pan,idx,step,16.)
            proj=blind_matrix(*lens,*f.m["size"],1.)
            focal=abs(proj[5])
            f.picture=observe(f.rgb,f.z,f.rev,f.mv,1/60,16.,(focal,-lens[1],1.))
            # Both candidates share the recovered lens and depth-unit convention.
            base=propose_ao(f.picture,previous is not None)
            if base is None: raise ValueError("No usable pan input")
            if exposure is None: exposure=.3/max(float(np.median(f.y[f.valid])),1e-4)
            originals.append(encode(f.rgb,exposure))
            ids.append(f"{pan.name}/{idx}")
            intel_delta=spatial_gpu(f,intel_tuning(base),fixture,scratch,matrix=proj)
            intel.append(encode(f.rgb*np.exp2(intel_delta[:,:,None]),exposure))
            options=[replace(base,denoise=1,temporal=0.), replace(base,denoise=2,temporal=0.),
                     replace(base,denoise=2,temporal=.58 if previous else 0.)]
            for i,opt in enumerate(options):
                link=(previous,previous_options[i]) if previous is not None and opt.temporal>0 else None
                delta=spatial_gpu(f,opt,fixture,scratch,link,proj)
                variants[i].append(encode(f.rgb*np.exp2(delta[:,:,None]),exposure))
                settings[i].append(asdict(opt))
            previous, previous_options=f,options
        add("Denoising in motion", "Which removes grain while keeping small contacts sharp?",
            originals,intel,variants[:2],settings[:2],ids)
        add("Temporal stability", "Which keeps shadows attached while the camera turns? Look for crawling or dark trails.",
            originals,intel,variants[1:],settings[1:],ids)
    identity=hashlib.sha256(json.dumps(trials,sort_keys=True).encode()).hexdigest()[:16]
    content=dict(version=1,id=identity,trials=trials,images=images)
    template=Path(__file__).with_name("ao_review.html").read_text(encoding="utf-8")
    output.write_text(template.replace("/*TRIAL_DATA*/null",json.dumps(content,separators=(",",":"))),encoding="utf-8")
    return trials


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("still1",type=Path)
    p.add_argument("still2",type=Path)
    p.add_argument("pan",type=Path)
    p.add_argument("fixture",type=Path)
    p.add_argument("output",type=Path)
    p.add_argument("--indices",type=int,nargs=2,required=True)
    p.add_argument("--lens",type=float,nargs=2,required=True,metavar=("VFOV","OFFSET"))
    p.add_argument("--step",type=int,default=2)
    a=p.parse_args()
    build(zip((a.still1,a.still2),a.indices),a.pan,a.lens,a.fixture,a.output,a.step)
