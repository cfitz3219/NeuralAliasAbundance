import unittest
import numpy as np
from tools.material_lab import corpus, render
from tools.material_lab.detail import unit
from tools.material_lab.learned_surface import combine, original_at_size, resize_wrap, slopes


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
        self.assertLessEqual(float(np.linalg.norm(slopes(out.normal)-slopes(base.normal),axis=-1).max()),.2200001)
        self.assertLessEqual(float(abs(out.roughness-base.roughness).max()),.1200001)
        self.assertGreater(float(abs(out.normal-base.normal).mean()),.001)
        np.testing.assert_array_equal(out.color,self.colour)
        np.testing.assert_allclose(np.linalg.norm(out.normal,axis=-1),1,atol=1e-12)

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


if __name__=='__main__':unittest.main()
