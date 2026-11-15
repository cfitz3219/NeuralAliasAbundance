"""Render the shipped Lab shader and compressed maps on a real OpenGL GPU.

Optional offline regression, not game/browser automation. Install moderngl in
the preparation environment. Run with a generated combined gallery and --output.
The sole shader translation is GLSL ES 300 -> desktop 330 version declaration.
Controller/UI behavior is covered separately by model_selector_test.cjs.
"""
import argparse
import base64
import io
import json
from pathlib import Path
import re
import numpy as np
from PIL import Image, ImageDraw


def read_payload(html):
    return json.loads(re.search(r'<script id="data" type="application/json">(.*?)</script>',html,re.S)[1])


class Renderer:
    def __init__(self,html,size=512):
        import moderngl
        self.gl=moderngl
        self.ctx=moderngl.create_standalone_context()
        self.size=size
        self.fbo=self.ctx.simple_framebuffer((size,size),components=4)
        self.fbo.use()
        vs=re.search(r'const vs=`(.*?)`;',html,re.S)[1].replace('#version 300 es','#version 330')
        fs=re.search(r'const fs=`(.*?)`;',html,re.S)[1].replace('#version 300 es','#version 330')
        self.program=self.ctx.program(vertex_shader=vs,fragment_shader=fs)
        self.vbo=self.ctx.buffer(np.array([-1,-1,1,-1,-1,1,-1,1,1,-1,1,1],dtype='f4').tobytes())
        self.vao=self.ctx.vertex_array(self.program,[(self.vbo,'2f','p')])
        self.textures=[]

    def bind(self,row,baseline=False):
        for tex in self.textures:tex.release()
        self.textures=[]
        original=row['original']['maps']
        surface=original if baseline else row['coordinated']['maps']
        colour=row['comparisons']['baseline']['maps']['base_color']
        sets=dict(colorMap=colour,normalMap=surface['normal'],roughMap=surface['roughness'],
                  authoredNormalMap=original['normal'],authoredRoughMap=original['roughness'])
        for i,(role,mips) in enumerate(sets.items()):
            images=[Image.open(io.BytesIO(base64.b64decode(s.split(',',1)[1]))).convert('RGBA') for s in mips]
            tex=self.ctx.texture(images[0].size,4,images[0].tobytes(),
                                 internal_format=0x8c43 if role=='colorMap' else 0x8058)
            tex.build_mipmaps(0,len(images)-1)
            for level,im in enumerate(images[1:],1):tex.write(im.tobytes(),level=level)
            tex.repeat_x=True;tex.repeat_y=True
            tex.filter=(self.gl.LINEAR_MIPMAP_LINEAR,self.gl.LINEAR)
            tex.use(i);self.program[role].value=i;self.textures.append(tex)

    def draw(self,strength,zoom=2,light=.15):
        values=dict(time=light,yaw=-18*np.pi/180,distanceLod=0.,surfaceStrength=strength,
                    curved=0,textureZoom=zoom)
        for key,value in values.items():self.program[key].value=value
        self.vao.render(self.gl.TRIANGLES)
        image=Image.frombytes('RGBA',(self.size,self.size),self.fbo.read(components=4))
        return np.asarray(image.transpose(Image.Transpose.FLIP_TOP_BOTTOM))[...,:3]

    def close(self):
        for tex in self.textures:tex.release()
        for obj in (self.vao,self.vbo,self.program,self.fbo,self.ctx):obj.release()


def difference(a,b):
    delta=np.abs(a.astype(float)-b)
    inside=np.any(a!=a[0,0],axis=-1)
    return dict(mean=float(delta[inside].mean()),p95=float(np.percentile(delta[inside],95)),
                fraction_changed_over_2=float((np.max(delta,axis=-1)[inside]>2).mean()))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('gallery',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    html=args.gallery.read_text(encoding='utf-8');rows=read_payload(html)
    args.output.mkdir(parents=True,exist_ok=True)
    r=Renderer(html);checks=[]
    try:
        print(r.ctx.info['GL_RENDERER'],flush=True)
        for row in rows:
            r.bind(row,baseline=True)
            references={zoom:r.draw(1,zoom) for zoom in (1,2)}
            r.bind(row)
            for zoom in (1,2):
                zero=r.draw(0,zoom);middle=r.draw(.65,zoom);full=r.draw(1,zoom)
                np.testing.assert_array_equal(zero,references[zoom])
                a=difference(zero,middle);b=difference(zero,full)
                # A nonzero bit is not evidence of a useful slider. Check the
                # actual full-strength signal on this fixed, lit synthetic set.
                assert b['p95']>=3,(row['name'],zoom,b)
                assert b['fraction_changed_over_2']>=.03,(row['name'],zoom,b)
                assert 0<a['mean']<b['mean'],(row['name'],zoom,a,b)
                checks.append(dict(material=row['name'],zoom=zoom,zero_matches_pbrify=True,
                                   strength_65=a,strength_100=b))
                if zoom==2:
                    sheet=Image.new('RGB',(r.size*2,r.size+40),'#11171b')
                    sheet.paste(Image.fromarray(zero),(0,40));sheet.paste(Image.fromarray(full),(r.size,40))
                    d=ImageDraw.Draw(sheet)
                    d.text((15,12),row['name'].replace('_',' ')+' | PBRify alone',fill='white')
                    d.text((r.size+15,12),'Same colour and light | Surface detail 100%',fill='white')
                    sheet.save(args.output/('Slider-'+row['name']+'.png'))
            print(row['name']+': zero exact; 65% and 100% visibly distinct at 1x/2x',flush=True)
        result=dict(renderer=r.ctx.info['GL_RENDERER'],shader='shipped gallery GLSL; version declaration adapted',
                    scope='Offline GPU; not browser UI or game quality',checks=checks)
        (args.output/'gpu-surface-check.json').write_text(json.dumps(result,indent=2)+'\n')
    finally:r.close()


if __name__=='__main__':main()
