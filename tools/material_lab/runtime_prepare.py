"""Prepare explicitly associated captured colour/normal pairs for the runtime.

Reuses the approved v4 surface recipe, PBRify models, content identities and DDS
cache. Unidentified packed maps remain in the game untouched. This adapter never
invents a roughness channel or publishes a flat stand-in. Model execution is
offline; the game only samples the resulting BC7 + BC5 textures.
"""
import argparse
import gc
import importlib.metadata
import json
import os
from pathlib import Path
import tempfile
import numpy as np
from PIL import Image
from . import cache, dds, runtime_source
from .detail import Material, linear, srgb, unit, mip_chain, down2
from .learned_surface import combine, SETTINGS, VERSION
from .model_runner import ModelUpscale


def read_pair(index, item, texconv):
    sources, decoded = {}, {}
    for role in ('base_color','normal'):
        source, raw = runtime_source.load_dump_entry(index,item[role])
        if (source['first_mip'] != 0 or source['first_array_slice'] != 0 or
                source['array_size'] != 1 or source['mip_count'] != max(source['width'],source['height']).bit_length()):
            raise ValueError('runtime preparation requires the full source mip chain')
        if role == 'base_color' and source['view_format'] not in runtime_source.SRGB:
            raise ValueError('this adapter preserves explicit sRGB colour views only')
        if role == 'normal' and source['view_format'] not in (83,84):
            raise ValueError('this adapter preserves two-channel tangent BC5 normals only')
        sources[role] = source
        decoded[role] = runtime_source.decode(source,raw,texconv)
    if decoded['base_color'].shape != decoded['normal'].shape:
        raise ValueError('associated colour/normal extent mismatch')
    if not item.get('association_evidence') or item.get('address') != 'wrap':
        raise ValueError('declare association evidence and periodic addressing')
    # Alpha-tested/transparent atlases need their own coverage and island rules.
    if np.min(decoded['base_color'][...,3]) != 1:
        raise ValueError('first runtime adapter is for opaque material pairs')
    xy = decoded['normal'][...,:2]
    if sources['normal']['view_format'] == 83:
        xy = xy*2-1
    n = unit(np.dstack((xy,np.sqrt(np.maximum(1e-8,1-np.sum(xy*xy,-1))))))
    rgba = decoded['base_color']
    rgba[...,:3] = linear(rgba[...,:3])
    return sources, Material(rgba,n,np.full(n.shape[:2],.5))


def descriptions(sources):
    return [dict(id='base_color',role='base_color',source=sources['base_color'],
                 channels=['color_r','color_g','color_b','opacity'],encoding='srgb',address='wrap'),
            dict(id='normal',role='normal',source=sources['normal'],
                 channels=['normal_x','normal_y','unused','unused'],address='wrap',
                 encoding='tangent_xy_'+('snorm' if sources['normal']['view_format']==84 else 'unorm')+'_positive_z')]


def predict_pair(original, sources, colour_model, normal_model, work):
    identity = dict(colour_source=sources['base_color'],
                    colour_model=cache.digest(colour_model.read_bytes()),
                    normal_model=cache.digest(normal_model.read_bytes()),
                    precision='float32',padding=16,tiling='whole-image',
                    fine_downsample=SETTINGS['fine_downsample'],medium_downsample=SETTINGS['medium_downsample'],
                    spandrel=importlib.metadata.version('spandrel'))
    key = cache.digest(cache.canonical(identity)); folder = work/key
    folder.mkdir(parents=True,exist_ok=True)
    colour_path = folder/'colour.png'
    if not colour_path.exists():
        runner=ModelUpscale(colour_model)
        if runner.scale != 4: raise ValueError('PBRify colour model must be 4x')
        rgb=np.round(srgb(original.color[...,:3])*255).clip(0,255).astype(np.uint8)
        output=runner(rgb,4,None,None,None)
        Image.fromarray(output).save(colour_path.with_suffix('.tmp.png'))
        os.replace(colour_path.with_suffix('.tmp.png'),colour_path)
        torch=runner.torch;del runner;gc.collect();torch.cuda.empty_cache()
    colour=np.asarray(Image.open(colour_path).convert('RGB'),dtype=np.uint8)
    if colour.shape[:2] != tuple(x*4 for x in original.normal.shape[:2]):
        raise ValueError('cached PBRify colour extent mismatch')
    paths=[];runner=None
    for scale in (SETTINGS['fine_downsample'],SETTINGS['medium_downsample']):
        path=folder/f'normal-{scale}.npy';paths.append(path)
        if path.exists():continue
        reduced=linear(colour.astype(np.float32)/255)
        for _ in range(scale.bit_length()-1):reduced=down2(reduced)
        rgb=np.round(srgb(reduced)*255).clip(0,255).astype(np.uint8);del reduced
        if runner is None:
            runner=ModelUpscale(normal_model)
            if runner.scale!=1 or runner.architecture!='SPAN':raise ValueError('expected PBRify 1x SPAN normals')
        n=runner(rgb,1,None,None,None).astype(np.float32)/127.5-1
        n[...,2]=np.maximum(n[...,2],.05);n=unit(n)
        with path.with_suffix('.tmp').open('wb') as f:np.save(f,n,allow_pickle=False)
        os.replace(path.with_suffix('.tmp'),path)
        del n,rgb
    if runner is not None:
        torch=runner.torch;del runner;gc.collect();torch.cuda.empty_cache()
    identity['artifacts']={p.name:cache.digest(p.read_bytes()) for p in [colour_path,*paths]}
    return colour,np.load(paths[0],allow_pickle=False),np.load(paths[1],allow_pickle=False),identity


def prepare(index,item,cache_root,work,texconv,colour_model,normal_model,strength=.65):
    sources,original=read_pair(index,item,texconv)
    big,fine,medium,identity=predict_pair(original,sources,colour_model,normal_model,work)
    h,w=big.shape[:2]
    colour=np.concatenate((linear(big.astype(np.float32)/255),np.ones((h,w,1),np.float32)),-1)
    # Roughness placeholders satisfy the shared math API only. Neither their
    # prediction nor their mip data is written; the game's packed maps stay bound.
    result=combine(original,colour,fine,np.full(fine.shape[:2],.5,np.float32),strength,
                   medium_normal=medium,medium_roughness=np.full(medium.shape[:2],.5,np.float32),
                   fine_downsample=SETTINGS['fine_downsample'])
    del big,fine,medium,colour;gc.collect()
    settings=dict(**SETTINGS,strength=strength,inference=identity,
                  association_evidence=item['association_evidence'],
                  scope='base colour and normal only; original packed material maps untouched')
    recipe=dict(schema_version=1,material_id=item['material_id'],algorithm='na-pbrify-captured-surface',
                algorithm_version=VERSION,settings=settings,seed=0,fixture=False,sources=descriptions(sources),
                encoder_sha256=cache.digest(texconv.read_bytes()),numpy_version=np.__version__)
    key=cache.digest(cache.canonical(recipe));target=cache_root/key
    if target.exists():return target,cache.validate(target)
    cache_root.mkdir(parents=True,exist_ok=True)
    chain=mip_chain(result)
    with tempfile.TemporaryDirectory(prefix='.preparing-',dir=cache_root) as temp:
        folder=Path(temp);maps=[]
        for desc in recipe['sources']:
            role=desc['role'];filename=role+'.dds'
            raw=dds.compress(chain,role,folder/filename,texconv,
                             signed_normal=role=='normal' and sources['normal']['view_format']==84)
            fmt,levels=dds.layout(raw)
            maps.append(dict(**desc,dxgi_format=fmt,width=w,height=h,file=filename,
                             sha256=cache.digest(raw),mips=levels))
        record={k:recipe[k] for k in ('schema_version','material_id','algorithm','algorithm_version','settings','seed','fixture')}
        record.update(entry_key=key,recipe=recipe,maps=maps,
                      payload_bytes=sum(sum(m['byte_size'] for m in desc['mips']) for desc in maps))
        (folder/'manifest.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        cache.validate(folder)
        os.rename(folder,target)
    return target,record


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('selection','cache','work','texconv','colour-model','normal-model'):
        p.add_argument('--'+name,type=Path,required=True)
    args=p.parse_args();selection=json.loads(args.selection.read_text(encoding='utf-8'))
    args.work.mkdir(parents=True,exist_ok=True)
    results=[]
    for item in selection['materials']:
        print('Preparing '+item['material_id'],flush=True)
        target,rec=prepare(Path(selection['capture_index']),item,args.cache,args.work,args.texconv.resolve(),
                           args.colour_model,args.normal_model)
        results.append(dict(material=rec['material_id'],entry=str(target.resolve()),payload_bytes=rec['payload_bytes']))
        (args.work/'receipt.json').write_text(json.dumps(results,indent=2)+'\n')
        print(json.dumps(results[-1]),flush=True)


if __name__=='__main__':main()
