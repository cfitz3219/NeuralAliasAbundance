"""Prepare PBRify colour + learned surface detail, reusing the Lab cache contract."""
import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import tempfile
import time
import numpy as np
from . import cache, dds, render, viewer
from .detail import Material, mip_chain, unit
from .learned_surface import combine, SETTINGS, VERSION
from .model_runner import ModelUpscale


def prepare(source, colour_entry, estimates, cache_root, texconv, normal_model, rough_model):
    source, colour_entry, cache_root, estimates = map(Path,(source,colour_entry,cache_root,estimates))
    metadata = json.loads((source/'source.json').read_text(encoding='utf-8'))
    if metadata.get('address') != 'wrap' or metadata.get('normal_convention') != 'tangent_xyz_positive_z':
        raise ValueError('learned detail v1 requires declared periodic positive-Z tangent materials')
    original_entry, original_record, _ = cache.prepare(source,cache_root,texconv,candidate='original')
    colour_record = cache.validate(colour_entry)
    viewer.check_baseline(original_record,colour_record)
    model_hashes = {role:cache.digest(Path(path).read_bytes()) for role,path in
                    (('normal',normal_model),('roughness',rough_model))}
    settings = dict(colour_entry=colour_record['entry_key'], authored_entry=original_record['entry_key'], models=model_hashes,
                    strength=1., normal_convention='DirectX: +Y along texture V',
                    **SETTINGS,
                    prediction_key=estimates.stem, prediction_sha256=cache.digest(estimates.read_bytes()))
    recipe = dict(schema_version=1,material_id=metadata['material_id'],algorithm='na-pbrify-authored-surface',
                  algorithm_version=VERSION,settings=settings,seed=metadata['seed'],fixture=metadata['fixture'],
                  sources=original_record['recipe']['sources'],
                  encoder_sha256=cache.digest(Path(texconv).read_bytes()),numpy_version=np.__version__)
    key = cache.digest(cache.canonical(recipe)); target = cache_root/key
    if target.exists():
        return target,cache.validate(target),True
    # Read the same source bytes identified by the original cache, through that cache.
    decoded = render.maps(original_entry)
    xy = decoded['normal'][...,:2]*2-1
    original = Material(decoded['base_color'],unit(np.dstack((xy,np.sqrt(np.maximum(1e-8,1-np.sum(xy*xy,-1)))))),decoded['roughness'][...,0])
    colour = render.maps(colour_entry)['base_color']
    with np.load(estimates,allow_pickle=False) as p:
        predicted_normal,predicted_roughness = p['normal'],p['roughness']
    result = combine(original,colour,predicted_normal,predicted_roughness)
    chain = mip_chain(result)
    cache_root.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.preparing-',dir=cache_root) as tmp:
        folder=Path(tmp); maps=[]
        for description in recipe['sources']:
            role=description['role']; filename=role+'.dds'
            if role=='base_color':
                # The two comparison arms must have byte-identical PBRify colour.
                colour_map=next(m for m in colour_record['maps'] if m['role']=='base_color')
                shutil.copyfile(colour_entry/colour_map['file'],folder/filename)
                raw=(folder/filename).read_bytes()
            else:
                raw=dds.compress(chain,role,folder/filename,texconv)
            fmt,levels=dds.layout(raw)
            maps.append(dict(**description,dxgi_format=fmt,width=levels[0]['width'],height=levels[0]['height'],
                             file=filename,sha256=cache.digest(raw),mips=levels))
        record={k:recipe[k] for k in ('schema_version','material_id','algorithm','algorithm_version','settings','seed','fixture')}
        record.update(entry_key=key,recipe=recipe,maps=maps,payload_bytes=sum(sum(m['byte_size'] for m in x['mips']) for x in maps))
        (folder/'manifest.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        cache.validate(folder)
        try: os.rename(folder,target)
        except OSError:
            if not target.exists(): raise
            cache.validate(target)
    return target,record,False


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('sources','colour-index','cache','predictions','index','texconv','normal-model','rough-model'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    colours=json.loads(args.colour_index.read_text(encoding='utf-8'))
    runners={role:ModelUpscale(path) for role,path in (('normal',args.normal_model),('roughness',args.rough_model))}
    if any(r.scale!=1 or r.architecture!='SPAN' for r in runners.values()):
        raise ValueError('this adapter expects the PBRify 1x SPAN surface models')
    identity=dict(models={role:cache.digest(p.read_bytes()) for role,p in
                          (('normal',args.normal_model),('roughness',args.rough_model))},
                  torch=runners['normal'].torch.__version__,spandrel=importlib.metadata.version('spandrel'),
                  precision='float32',padding=16,architecture='SPAN',normal_convention='DirectX')
    args.predictions.mkdir(parents=True,exist_ok=True)
    index={}; receipts=[]
    for source in sorted(p for p in args.sources.iterdir() if (p/'source.json').is_file()):
        start=time.perf_counter(); name=json.loads((source/'source.json').read_text())['material_id']
        colour_entry=(args.colour_index.parent/colours[name]).resolve()
        rec=cache.validate(colour_entry)
        prediction_key=cache.digest(cache.canonical(dict(colour=rec['entry_key'],**identity)))
        prediction=args.predictions/(prediction_key+'.npz')
        if not prediction.exists():
            descriptor=next(m for m in rec['maps'] if m['role']=='base_color')
            rgb=np.asarray(next(viewer.decoded_mips(colour_entry,descriptor)),dtype=np.uint8)[...,:3]
            normal=runners['normal'](rgb,1,None,None,None).astype(np.float64)/127.5-1
            # Author's pipeline labels this output normal_dx. Our +V tangent basis matches it.
            normal[...,2]=np.maximum(normal[...,2],.05); normal=unit(normal)
            rough=runners['roughness'](rgb,1,None,None,None).astype(np.float64).mean(axis=-1)/255
            with tempfile.NamedTemporaryFile(dir=args.predictions,suffix='.npz',delete=False) as tmp:
                np.savez_compressed(tmp,normal=normal.astype(np.float32),roughness=rough.astype(np.float32))
                temp=Path(tmp.name)
            os.replace(temp,prediction)
        entry,record,hit=prepare(source,colour_entry,prediction,args.cache,args.texconv,args.normal_model,args.rough_model)
        index[name]=str(entry.resolve())
        receipts.append(dict(material=name,entry=str(entry.resolve()),prediction=str(prediction.resolve()),
                             seconds=round(time.perf_counter()-start,3),reused=hit))
        print(json.dumps(receipts[-1]),flush=True)
    args.index.parent.mkdir(parents=True,exist_ok=True)
    args.index.write_text(json.dumps(index,indent=2)+'\n',encoding='utf-8')
    args.index.with_suffix('.receipt.json').write_text(json.dumps(dict(identity=identity,materials=receipts),indent=2)+'\n',encoding='utf-8')


if __name__=='__main__': main()
