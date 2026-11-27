import unittest
import numpy as np
from form_lighting import FormReferee, Budget


def fixture():
    h,w=96,240
    y,x=np.indices((h,w),dtype=np.float32)
    z=.35+.055*np.cos(x/7)+.025*np.cos(y/11)
    lum=.28*(1+.15*np.cos(x/10)+.18*np.cos(y/15))
    return np.repeat(lum[...,None],3,axis=2),z


class FormTests(unittest.TestCase):
    def test_flat_paint_plane_and_slab(self):
        rgb,z=fixture()
        y,x=np.indices(z.shape,dtype=np.float32)
        for plane in [np.full_like(z,.4), .35+.0003*x+.00012*y, np.where(x<130,.28,.48).astype("f4")]:
            a,c,_=FormReferee().layers(rgb,plane,True)
            self.assertLessEqual(abs(a+c).max(),1e-6)

    def test_layers_budgets_and_lighting_visibility(self):
        rgb,z=fixture()
        mv=np.zeros((*z.shape,2),dtype="f4")
        a,c,s=FormReferee().layers(rgb,z,True,mv)
        self.assertLessEqual(a.max(),0)
        self.assertTrue((c>0).any())
        self.assertFalse(((a<0)&(c<0)).any())
        self.assertAlmostEqual(s["near_black_before"],s["near_black_after"])
        self.assertLessEqual(s["mean_dark"],Budget().mean+1e-6)
        self.assertLessEqual(s["tile_dark"],Budget().tile+1e-6)
        self.assertTrue(.92<=s["contrast"]<=1.10)
        self.assertGreater(s["share_quarter_stop"],.20)
        for cd,fd in [(1,0),(0,1),(0,0)]:
            a,c,_=FormReferee().layers(rgb,z,True,mv,cd,fd)
            if not cd: self.assertFalse(a.any())
            if not fd: self.assertFalse(c.any())
            if not cd and not fd:
                self.assertTrue(np.array_equal(rgb,rgb*np.exp2((a+c)[...,None])))

    def test_depth_conventions_sky_black_and_no_camera(self):
        rgb,z=fixture()
        z[:8]=0
        rgb[50:55]=.003
        a,c,s=FormReferee().layers(rgb,z,True)
        aa,cc,_=FormReferee().layers(rgb,1-z,False)
        np.testing.assert_allclose(a+c,aa+cc,atol=2e-4)
        self.assertFalse((a+c)[:8].any())
        self.assertFalse((a+c)[50:55].any())
        self.assertIsNone(s["speed"])
        self.assertEqual(FormReferee().layers(rgb,z,None)[2]["state"],"no reliable input")

    def test_live_controls_and_motion_unknown(self):
        rgb,z=fixture()
        mv=np.zeros((*z.shape,2),dtype=np.float32)
        brain=FormReferee()
        still=brain.layers(rgb,z,True,mv)[2]["controls"]
        mv[...,0]=3
        for _ in range(12):
            turning=brain.layers(rgb,z,True,mv)[2]["controls"]
        self.assertLess(turning["radius"],still["radius"])
        self.assertGreater(turning["radius"],0)
        self.assertEqual(brain.layers(rgb,z,True,mv,frame_ms=24)[2]["controls"]["directions"],2)
        a,c,s=brain.layers(rgb,z,True,mv,gpu_ms=1.5)
        self.assertEqual(s["state"],"time allowance")
        self.assertFalse((a+c).any())
        mv.fill(65504)
        self.assertIsNone(brain.layers(rgb,z,True,mv)[2]["speed"])


if __name__=="__main__":
    unittest.main()
