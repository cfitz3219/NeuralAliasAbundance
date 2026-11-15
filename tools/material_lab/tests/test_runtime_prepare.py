import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import numpy as np
from tools.material_lab import cache, dds, runtime_source, runtime_prepare, corpus
from tools.material_lab.detail import mip_chain, unit
from tools.material_lab.learned_surface import resize_wrap
from test_runtime_source import write_dump


class GameMaterialPreparation(unittest.TestCase):
    def setUp(self):
        tool=os.environ.get('NA_TEXCONV')
        if not tool:self.skipTest('set NA_TEXCONV for signed compression/decoding')
        self.texconv=Path(tool)
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)

    def test_signed_normal_roundtrip_retains_negative_components_and_mips(self):
        m=corpus.make('masonry',12,32)
        m.normal[:16]=unit(np.array([-.55,.3,1.]))
        m.normal[16:]=unit(np.array([.4,-.6,1.]))
        data=dds.compress(mip_chain(m),'normal',self.root/'normal.dds',self.texconv,signed_normal=True)
        fmt,levels=dds.layout(data)
        self.assertEqual(fmt,84);self.assertEqual(len(levels),6)
        for level,original in zip(levels,mip_chain(m)):
            b=data[level['offset']:level['offset']+level['byte_size']]
            xy=dds.decode_signed_bc5(b,level['width'],level['height'],self.texconv)
            np.testing.assert_allclose(xy,original.normal[...,:2],atol=.018)
        self.assertLess(float(xy[...,1].min()),0)

    def test_srgb_alias_decodes_storage_without_gamma_or_channel_change(self):
        m=corpus.make('masonry',13,32)
        data=dds.compress([m],'base_color',self.root/'c.dds',self.texconv)
        desc=dict(width=32,height=32,first_mip=0,view_format=99)
        a=runtime_source.decode(desc,data[148:])
        desc['view_format']=98
        np.testing.assert_array_equal(a,runtime_source.decode(desc,data[148:]))

    def test_actual_captured_pair_to_cache_preserves_signed_identity_and_omits_packed(self):
        m=corpus.make('masonry',15,32)
        m.color[...,3]=1
        chain=mip_chain(m);digests={}
        for role in ('base_color','normal'):
            if role=='base_color':
                # Opaque source bytes (BC7 may quantize alpha255 to254).
                fmt=29
                payloads=[(0,i,dds.pixels(level,role).tobytes()) for i,level in enumerate(chain)]
            else:
                raw=dds.compress(chain,role,self.root/(role+'.dds'),self.texconv,signed_normal=True)
                fmt,levels=dds.layout(raw)
                payloads=[(0,v['level'],raw[v['offset']:v['offset']+v['byte_size']]) for v in levels]
            index,digest=write_dump(self.root/'capture',(fmt,fmt,32,32,0,len(payloads),0,1),payloads)
            digests[role]=digest
        item=dict(material_id='captured-test',**digests,address='wrap',association_evidence='known synthetic matched pair')
        sources,original=runtime_prepare.read_pair(index,item,self.texconv)
        h=128;rgb=np.round(resize_wrap(m.color,h,h)[...,:3]*255).astype('uint8')
        y,x=np.mgrid[:64,:64];fine=unit(np.stack((.15*np.sin(x),.12*np.cos(y),np.ones_like(x)),-1))
        medium=resize_wrap(fine,32,32)
        fake=dict(artifacts={'test':'fixture'})
        # Exercise real encode/decode/publication/identity; inference math already
        # has separate approved-model evidence. No fake inference claim here.
        with patch.object(runtime_prepare,'predict_pair',return_value=(rgb,fine,medium,fake)):
            folder,record=runtime_prepare.prepare(index,item,self.root/'cache',self.root,self.texconv,Path('unused'),Path('unused'))
        self.assertEqual(len(record['maps']),2)
        self.assertEqual({v['role'] for v in record['maps']},{'base_color','normal'})
        normal=record['maps'][1]
        self.assertEqual(normal['dxgi_format'],84)
        self.assertEqual(normal['encoding'],'tangent_xy_snorm_positive_z')
        self.assertEqual(normal['source'],sources['normal'])
        self.assertEqual(len(normal['mips']),sources['normal']['mip_count']+2)
        cache.validate(folder)
        record['maps'][1]['encoding']='tangent_xy_unorm_positive_z'
        (folder/'manifest.json').write_text(json.dumps(record))
        with self.assertRaises(ValueError):cache.validate(folder)

    def test_striped_resize_matches_single_grid(self):
        from tools.material_lab.render import sample
        a=np.random.default_rng(5).normal(size=(16,32,3))
        x,y=np.meshgrid((np.arange(128)+.5)/128,(np.arange(128)+.5)/128)
        np.testing.assert_array_equal(resize_wrap(a,128,128),sample(a,np.stack((x,y),-1)))


if __name__=='__main__':unittest.main()
