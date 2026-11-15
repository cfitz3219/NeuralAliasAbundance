import unittest
import numpy as np
from tools.material_lab import corpus, render
from tools.material_lab.detail import unit
from tools.material_lab.learned_surface import combine, original_at_size, resize_wrap, slopes, SETTINGS


class AuthoredSurface(unittest.TestCase):
    def setUp(self):
        self.original=corpus.make('masonry',51,32)
        self.colour=resize_wrap(self.original.color,128,128)
        y,x=np.mgrid[:128,:128]
        self.prediction=unit(np.stack([.18*np.sin(x*2*np.pi/6),.14*np.cos(y*2*np.pi/5),np.ones_like(x)],-1))
        self.rough=.5+.15*np.sin(x*2*np.pi/7)

    def test_zero_preserves_authored_surface_colour_and_opacity(self):
        before=self.original.copy()
        out=combine(self.original,self.colour,self.prediction,self.rough,0)
        base=original_at_size(self.original,self.colour)
        np.testing.assert_array_equal(out.color,self.colour)
        np.testing.assert_array_equal(out.normal,base.normal)
        np.testing.assert_array_equal(out.roughness,base.roughness)
        np.testing.assert_array_equal(self.original.normal,before.normal)

    def test_constant_prediction_cannot_flatten_or_tilt_original(self):
        prediction=np.broadcast_to(unit(np.array([.3,-.2,1.])),(128,128,3))
        out=combine(self.original,self.colour,prediction,np.full((128,128),.1))
        base=original_at_size(self.original,self.colour)
        np.testing.assert_allclose(out.normal,base.normal,atol=1e-12)
        np.testing.assert_allclose(out.roughness,base.roughness,atol=1e-12)

    def test_protected_region_is_unchanged_and_addition_is_bounded(self):
        mask=np.ones((128,128)); mask[:,30:60]=0
        base=original_at_size(self.original,self.colour)
        out=combine(self.original,self.colour,self.prediction,self.rough,mask=mask)
        np.testing.assert_array_equal(out.normal[:,30:60],base.normal[:,30:60])
        np.testing.assert_array_equal(out.roughness[:,30:60],base.roughness[:,30:60])
        self.assertLessEqual(float(np.linalg.norm(slopes(out.normal)-slopes(base.normal),axis=-1).max()),SETTINGS['max_added_slope']+1e-7)
        self.assertLessEqual(float(abs(out.roughness-base.roughness).max()),.1200001)
        self.assertGreater(float(abs(out.normal-base.normal).mean()),.001)
        np.testing.assert_array_equal(out.color,self.colour)
        np.testing.assert_allclose(np.linalg.norm(out.normal,axis=-1),1,atol=1e-12)

    def test_learned_relief_cannot_flatten_an_authored_groove(self):
        base=original_at_size(self.original,self.colour)
        bs=slopes(base.normal)
        out=combine(self.original,self.colour,self.prediction,self.rough)
        added=slopes(out.normal)-bs
        strong=np.sum(bs*bs,-1)>SETTINGS['protected_slope_threshold']**2
        self.assertGreater(int(strong.sum()),100)
        self.assertGreaterEqual(float(np.sum(added*bs,-1)[strong].min()),-1e-12)
        self.assertGreater(float(np.linalg.norm(added,axis=-1)[strong].mean()),.01)

    def test_periodic_shift_does_not_change_detail_response(self):
        original=self.original.copy()
        for field in ('color','normal','roughness'):
            setattr(original,field,np.roll(getattr(original,field),3,axis=1))
        expected=combine(self.original,self.colour,self.prediction,self.rough)
        shifted=combine(original,np.roll(self.colour,12,axis=1),np.roll(self.prediction,12,axis=1),np.roll(self.rough,12,axis=1))
        np.testing.assert_allclose(shifted.normal,np.roll(expected.normal,12,axis=1),atol=1e-12)
        np.testing.assert_allclose(shifted.roughness,np.roll(expected.roughness,12,axis=1),atol=1e-12)

    def test_invalid_prediction_does_not_publish_nonsense(self):
        bad=self.prediction.copy();bad[0,0]=np.nan
        with self.assertRaises(ValueError):combine(self.original,self.colour,bad,self.rough)
        with self.assertRaises(ValueError):combine(self.original,self.colour,-self.prediction,self.rough)

    def test_zero_slider_has_identical_rendered_pixels_to_colour_only(self):
        # Exercise actual reference renderer at different angles and lights, including
        # unequal original/enhanced map sizes. Zero must not use resampled surface maps.
        original={'base_color':[self.colour], 'normal':[self.original.normal*.5+.5],
                  'roughness':[self.original.roughness[...,None]]}
        out=combine(self.original,self.colour,self.prediction,self.rough)
        combined={**original,'normal':[out.normal*.5+.5],'roughness':[out.roughness[...,None]],
                  'authored_normal':original['normal'],'authored_roughness':original['roughness'],
                  'surface_strength':0.}
        for angle in (-40,0,35):
            for light in (.15,1.35):
                a=np.asarray(render.render(original,64,light,angle))
                b=np.asarray(render.render(combined,64,light,angle))
                np.testing.assert_array_equal(a,b)

    def test_medium_scale_preserves_base_and_survives_minification(self):
        from tools.material_lab.detail import mip_chain, Material
        y,x=np.mgrid[:32,:32]
        medium=unit(np.stack([.06*np.sin(x*2*np.pi/8),.04*np.cos(y*2*np.pi/8),np.ones_like(x)],-1))
        rough=.5+.03*np.sin(x*2*np.pi/8)
        # Flat authored material isolates frequency survival from existing grooves.
        original=Material(self.original.color,np.broadcast_to([0.,0.,1.],(32,32,3)).copy(),np.full((32,32),.6))
        flat=np.broadcast_to([0.,0.,1.],(128,128,3)).copy()
        fine=combine(original,self.colour,flat,np.full((128,128),.5))
        both=combine(original,self.colour,flat,np.full((128,128),.5),medium_normal=medium,medium_roughness=rough)
        np.testing.assert_array_equal(both.color,self.colour)
        coarse=mip_chain(both)[2];old=mip_chain(fine)[2]
        self.assertGreater(float(abs(coarse.normal-old.normal).mean()),.02)
        self.assertLess(float(abs(coarse.roughness.mean()-.6)),.02)
        zero=combine(original,self.colour,self.prediction,self.rough,0,medium_normal=medium,medium_roughness=rough)
        np.testing.assert_array_equal(zero.normal,fine.normal)
        np.testing.assert_array_equal(zero.roughness,fine.roughness)

    def test_medium_cannot_replace_original_shape_or_overrun_detail_budget(self):
        medium=self.prediction[::4,::4];rough=self.rough[::4,::4]
        base=original_at_size(self.original,self.colour)
        mask=np.ones((128,128));mask[:,:16]=0
        out=combine(self.original,self.colour,self.prediction,self.rough,mask=mask,
                    medium_normal=medium,medium_roughness=rough)
        added=slopes(out.normal)-slopes(base.normal)
        self.assertLessEqual(float(np.linalg.norm(added,axis=-1).max()),SETTINGS['max_added_slope']+1e-7)
        self.assertLessEqual(float(abs(out.roughness-base.roughness).max()),.1200001)
        strong=np.sum(slopes(base.normal)**2,-1)>SETTINGS['protected_slope_threshold']**2
        self.assertGreaterEqual(float(np.sum(added*slopes(base.normal),-1)[strong].min()),-1e-12)
        np.testing.assert_array_equal(out.normal[:,:16],base.normal[:,:16])
        np.testing.assert_array_equal(out.roughness[:,:16],base.roughness[:,:16])
        constant=np.broadcast_to(unit(np.array([.3,-.2,1.])),(32,32,3))
        a=combine(self.original,self.colour,self.prediction,self.rough)
        b=combine(self.original,self.colour,self.prediction,self.rough,
                  medium_normal=constant,medium_roughness=np.full((32,32),.2))
        np.testing.assert_allclose(a.normal,b.normal,atol=1e-12)
        np.testing.assert_allclose(a.roughness,b.roughness,atol=1e-12)

    def test_medium_prediction_pair_is_validated(self):
        for n,r in [(self.prediction,None),(self.prediction,self.rough),
                    (np.full((32,32,3),np.nan),self.rough[::4,::4]),
                    (-self.prediction[::4,::4],self.rough[::4,::4])]:
            with self.assertRaises(ValueError):
                combine(self.original,self.colour,self.prediction,self.rough,medium_normal=n,medium_roughness=r)

    def test_half_size_fine_prediction_doubles_grain_spacing_not_strength(self):
        from tools.material_lab.detail import Material
        original=Material(self.original.color,np.broadcast_to([0.,0.,1.],(32,32,3)).copy(),np.full((32,32),.6))
        peaks=[];amplitudes=[]
        for scale in (1,2):
            side=128//scale;y,x=np.mgrid[:side,:side]
            n=unit(np.stack([.025*np.sin(x*2*np.pi/8),np.zeros_like(x),np.ones_like(x)],-1))
            r=.5+.015*np.sin(x*2*np.pi/8)
            out=combine(original,self.colour,n,r,fine_downsample=scale)
            signal=slopes(out.normal)[64,:,0]
            peaks.append(int(np.argmax(abs(np.fft.rfft(signal))[1:]))+1)
            amplitudes.append(float(np.max(abs(signal))))
            self.assertEqual(int(np.argmax(abs(np.fft.rfft(out.roughness[64]-.6))[1:]))+1,peaks[-1])
            np.testing.assert_array_equal(out.color,self.colour)
            zero=combine(original,self.colour,n,r,0,fine_downsample=scale)
            np.testing.assert_array_equal(zero.normal,original_at_size(original,self.colour).normal)
        self.assertEqual(peaks,[16,8])
        self.assertGreater(amplitudes[1]/amplitudes[0],.9)
        self.assertLessEqual(amplitudes[1]/amplitudes[0],1.01)

    def test_half_size_detail_stays_aligned_to_material_uvs(self):
        n=self.prediction[::2,::2];r=self.rough[::2,::2]
        original=self.original.copy()
        for field in ('color','normal','roughness'):
            setattr(original,field,np.roll(getattr(original,field),3,axis=1))
        a=combine(self.original,self.colour,n,r,fine_downsample=2)
        b=combine(original,np.roll(self.colour,12,axis=1),np.roll(n,6,axis=1),np.roll(r,6,axis=1),fine_downsample=2)
        np.testing.assert_allclose(b.normal,np.roll(a.normal,12,axis=1),atol=1e-12)
        np.testing.assert_allclose(b.roughness,np.roll(a.roughness,12,axis=1),atol=1e-12)


if __name__=='__main__':unittest.main()
