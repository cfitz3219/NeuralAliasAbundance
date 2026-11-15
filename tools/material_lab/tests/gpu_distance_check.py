"""Compare prepared detail scales using the actual gallery shader and GPU.

Fixed synthetic corpus regression. Pixel differences demonstrate delivery, not
physical accuracy or universal perceptual quality. Saves images for inspection.
"""
import argparse
import json
from pathlib import Path
import re
import numpy as np
from PIL import Image, ImageDraw
from .gpu_surface_check import Renderer, read_payload, difference


def run(previous, current, output):
    old_html=previous.read_text(encoding='utf-8');html=current.read_text(encoding='utf-8')
    old={row['name']:row for row in read_payload(old_html)};rows=read_payload(html)
    # This change must be in the prepared maps, not brighter preview lighting,
    # forced mip selection, different camera or a more expensive shader.
    for stage in ('vs','fs'):
        pattern=rf'const {stage}=`(.*?)`;'
        assert re.search(pattern,old_html,re.S)[1]==re.search(pattern,html,re.S)[1]
    output.mkdir(parents=True,exist_ok=True)
    checks=[]
    for size in (256,512):
        gpu=Renderer(html,size)
        try:
            for row in rows:
                before=old[row['name']]
                assert row['original']==before['original']
                assert row['comparisons']['baseline']==before['comparisons']['baseline']
                assert row['coordinated']['bytes']==before['coordinated']['bytes']
                assert row['coordinated']['dimensions']==before['coordinated']['dimensions']
                gpu.bind(row,True)
                references={z:gpu.draw(1,z) for z in (1,2,4)}
                gpu.bind(before)
                previous_images={z:gpu.draw(.65,z) for z in (1,2,4)}
                gpu.bind(row)
                sheet=Image.new('RGB',(size*3,(size+40)*3),'#11171b');d=ImageDraw.Draw(sheet)
                for j,z in enumerate((1,2,4)):
                    zero=gpu.draw(0,z);half=gpu.draw(.65,z);full=gpu.draw(1,z)
                    np.testing.assert_array_equal(zero,references[z])
                    a=difference(zero,previous_images[z]);b=difference(zero,half)
                    assert 0<b['mean']<difference(zero,full)['mean']
                    if z==1:
                        assert b['mean']>a['mean']*1.2,(row['name'],size,a,b)
                        assert b['p95']>=5,(row['name'],size,b)
                    checks.append(dict(material=row['name'],panel_pixels=size,zoom=z,
                                       previous_65=a,current_65=b,zero_exact=True))
                    for i,(label,pixels) in enumerate((('PBRify alone',zero),('Previous fine detail',previous_images[z]),('Fine + medium detail',half))):
                        sheet.paste(Image.fromarray(pixels),(i*size,j*(size+40)+40))
                        d.text((i*size+12,j*(size+40)+12),f'{label} | {z}x',fill='white')
                # Cross three mip boundaries in tiny steps: no mode switch/pop.
                mip_steps=[]
                for lod in (1.,2.,3.):
                    a=gpu.draw(.65,1,lod=lod-.001)
                    b=gpu.draw(.65,1,lod=lod+.001)
                    delta=abs(a.astype(float)-b)
                    assert delta.max()<=2,(row['name'],size,lod,delta.max())
                    mip_steps.append(dict(lod=lod,maximum_channel_step=float(delta.max())))
                checks.append(dict(material=row['name'],panel_pixels=size,mip_boundary_steps=mip_steps))
                if size==512:sheet.save(output/('Distance-'+row['name']+'.png'))
                print(f'{row["name"]}: {size}px, 1x/2x/4x and mip transitions pass',flush=True)
            renderer=gpu.ctx.info['GL_RENDERER']
        finally:gpu.close()
    result=dict(renderer=renderer,scope='offline GPU synthetic materials; not browser or game proof',
                unchanged_shader=True,unchanged_texture_bytes=True,checks=checks)
    (output/'gpu-distance-check.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--previous',type=Path,required=True)
    p.add_argument('--current',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();run(a.previous,a.current,a.output)
