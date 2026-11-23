import unittest
import numpy as np
from contact_shadow import Referee, guard_budget


def scene(h=144,w=360):
    y,x=np.indices((h,w))
    dist=np.abs((x%24)-12)
    depth=(.28+.0038*dist+.00002*y).astype('f4')
    # Each group of four geometrically identical creases includes one washed-out one.
    shadow=(np.floor_divide(x,24)%4!=1)
    dip=np.maximum(0,1-dist/5)*shadow*.32
    lum=.4*np.exp2(-dip)
    return np.repeat(lum[...,None],3,axis=2).astype('f4'),depth,dist,shadow


class ContactTests(unittest.TestCase):
    def test_same_geometry_two_lighting_treatments(self):
        rgb,z,dist,shaded=scene()
        delta,credit,report=Referee().evaluate(rgb,z,True,np.zeros((*z.shape,2),np.float32))
        stripe=(dist==0)&(np.indices(z.shape)[0]>12)&(np.indices(z.shape)[0]<132)
        self.assertLess(float(delta[stripe&~shaded].mean()),-.015)
        self.assertLess(float(np.abs(delta[stripe&shaded]).mean()),.003)
        self.assertGreater(float(credit[stripe&shaded].mean()),.05)
        self.assertAlmostEqual(report["near_black_before"],report["near_black_after"])
        print("equal geometry, mean added stops (washed, already shaded)",-delta[stripe&~shaded].mean(),-delta[stripe&shaded].mean())

    def test_no_teacher_flat_and_depth_edges(self):
        rgb,z,_,_=scene();rgb.fill(.4)
        self.assertFalse(Referee().evaluate(rgb,z,True)[0].any())
        h,w=z.shape
        ramp=np.broadcast_to(np.linspace(.15,.45,w,dtype='f4'),(h,w)).copy()
        rgb,_,_,_=scene()
        for q in (ramp,(ramp>.30).astype('f4')*.2+.1):
            self.assertFalse(Referee().evaluate(rgb,q,True)[0].any())

    def test_normal_and_reversed_and_zero(self):
        rgb,z,_,_=scene()
        a=Referee().evaluate(rgb,z,True)[0]
        b=Referee().evaluate(rgb,1-z,False)[0]
        np.testing.assert_allclose(a,b,atol=1e-5)
        for kwargs in ({"dial":0},{"gpu_ms":4},{"frame_ms":26}):
            d=Referee().evaluate(rgb,z,True,**kwargs)[0]
            self.assertFalse(d.any())
            self.assertTrue(np.array_equal(rgb*np.exp2(d[...,None]),rgb))
        self.assertFalse(Referee().evaluate(rgb,z,None)[0].any())

    def test_restart_cut_and_motion_response(self):
        rgb,z,_,_=scene()
        ai=Referee(); m=np.zeros((*z.shape,2),'f4')
        static=ai.evaluate(rgb,z,True,m)[2]["settings"]
        yy,xx=np.indices(z.shape); m[...,0]=-2
        for n in range(1,15):
            d,g,s=ai.evaluate(np.roll(rgb,2*n,axis=1),np.roll(z,2*n,axis=1),True,m,adjacent=True)
            self.assertLessEqual(s["worst_tile_stops"],.040001)
            self.assertLessEqual(s["mean_stops"],.010001)
        self.assertLess(s["settings"]["reach"],static["reach"])
        self.assertLess(s["settings"]["history"],static["history"])
        self.assertEqual(ai.evaluate(rgb,z,True,m,cut=True,adjacent=True)[2]["history_pixels"],0)
        _,_,low=ai.evaluate(rgb,z,True,m,gpu_ms=1.49,adjacent=True)
        _,_,budget=ai.evaluate(rgb,z,True,m,frame_ms=24,adjacent=True)
        self.assertEqual(budget['settings']['sample_axes'],2)
        self.assertLess(low["settings"]["diagonal_weight"],s["settings"]["diagonal_weight"])

    def test_motion_sentinels_and_shared_guard(self):
        rgb,z,_,_=scene()
        ai=Referee()
        bad=np.full((*z.shape,2),65504,np.float32)
        d,credit,report=ai.evaluate(rgb,z,True,bad)
        self.assertIsNone(report["motion_heights_per_second"])
        self.assertEqual(report["history_pixels"],0)
        self.assertIsNone(ai.previous)
        off,shared,_=ai.evaluate(rgb,z,True,bad,dial=0,need_shared_guard=True)
        self.assertFalse(off.any())
        np.testing.assert_array_equal(shared,credit)
        self.assertTrue(shared.any())

    def test_stricter_budget_and_shadow_credit(self):
        rgb,z,_,_=scene(); ai=Referee()
        a,_,report=ai.evaluate(rgb,z,True,mean_cap=.002,tile_cap=.010)
        self.assertLessEqual(report["mean_stops"],.0020001)
        self.assertLessEqual(report["worst_tile_stops"],.0100001)
        self.assertLessEqual(a.max(),0)
        y=np.linspace(.003,.007,400,dtype="f4").reshape(20,20)
        _,s=guard_budget(np.full_like(y,-1),y,np.ones(y.shape,bool),1.,1.)
        self.assertEqual(s["near_black_before"],s["near_black_after"])


if __name__=="__main__":
    unittest.main()
