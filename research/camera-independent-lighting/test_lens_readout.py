"""Ground truth comes from 3D reprojection, independent of the fitted polynomial."""
import unittest
import numpy as np
from lens_readout import fit_field, readout
from recorded_suite import run
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from calibration import score_layer

class ReadoutTests(unittest.TestCase):
    def sequence(self,kind,seed=5, focal=2.65):
        rng = np.random.default_rng(seed)
        fields = []
        for k in range(18):
            x,y=rng.uniform(-1.65,1.65,900),rng.uniform(-1,1,900)
            q=rng.uniform(.008,.15,len(x))
            z=1/(q+.002)
            xyz=np.column_stack((x*z/focal,y*z/focal,z))
            turn=kind in ("turn","mixed","yaw")
            pitch=(.0025+.00035*np.cos(k)) if turn and kind!="yaw" else 0
            yaw=(.003+.0003*np.sin(k)) if turn else 0
            cx,sx,cy,sy=np.cos(pitch),np.sin(pitch),np.cos(yaw),np.sin(yaw)
            rot=np.array([[cy,sy*sx,sy*cx],[0,cx,-sx],[-sy,cy*sx,cy*cx]])
            trans=[.35*np.cos(.4*k),.30*np.sin(.6*k),.03] if kind in ("slide","mixed") else [0,0,0]
            prev=xyz@rot.T+trans
            flow=720*(focal*prev[:,:2]/prev[:,2:3]-np.column_stack((x,y)))
            flow+=rng.normal(0,.015,flow.shape)
            flow[:90]+=rng.normal(0,7,(90,2))  # independently moving foreground
            a,_=fit_field(x,y,q,flow)
            if a is not None: fields.append(a)
        return fields

    def test_independent_rigid_reprojections(self):
        a=readout(self.sequence("mixed"))
        self.assertEqual(a["state"],"candidate",a)
        self.assertLess(abs(a["focal_normalized"]/2.65-1),.01)
        self.assertLess(abs(a["depth_offset"]/.002-1),.10)
        print("known mixed motion:", a)

    def test_flat_map_control_and_shooter_turn(self):
        slide=self.sequence("slide")
        self.assertEqual(readout(slide)["state"],"no lens yet")
        turn=self.sequence("turn")
        partial=readout(turn)
        self.assertEqual(partial["state"],"no lens yet")
        self.assertLess(abs(partial["rotation_only"]["focal_normalized"]/2.65-1),.01)
        other=readout(self.sequence("turn",seed=17))
        self.assertLess(abs(partial["rotation_only"]["focal_normalized"]/other["rotation_only"]["focal_normalized"]-1),.01)
        both=readout(turn,slide)
        self.assertEqual(both["state"],"candidate",both)
        self.assertLess(abs(both["depth_offset"]/.002-1),.10)
        print("rotation only:",partial,"with strategy slide:",both)

    def test_static_one_axis_and_zoom(self):
        self.assertEqual(readout(self.sequence("still"))["state"],"no lens yet")
        self.assertEqual(readout(self.sequence("yaw"))["state"],"no lens yet")
        self.assertEqual(readout(self.sequence("mixed")+self.sequence("mixed",focal=3.5))["state"],"no lens yet")

    def test_recording_modes_and_incomplete_retry(self):
        with TemporaryDirectory() as directory:
            root=Path(directory)
            for kind in ("still","turn1","turn2","slide"):
                dest=root/("j2-lighting-"+kind+"-01")
                dest.mkdir()
                tag=dict(exe="test", pid=5, build_commit="build",process_started_filetime=123,kind="j2-lighting-"+kind)
                (dest/"session.json").write_text(json.dumps(tag))
                for i in range(30):
                    planes=[]
                    for name in ("color","depth","motion","dlssout","present"):
                        (dest/f"{i:05d}-{name}.raw").write_bytes(b"1234")
                        planes.append(dict(name=name,bytes=4))
                    m=dict(exe="test",pid=5,index=i,size=[1,1],depth_reversed=0,evaluation_succeeded=True,
                           present_pairing="own_output",planes=planes)
                    (dest/f"{i:05d}-meta.json").write_text(json.dumps(m))
            retry=root/"j2-lighting-turn1-02"
            retry.mkdir()
            (retry/"session.json").write_bytes((root/"j2-lighting-turn1-01"/"session.json").read_bytes())
            free=run(root,"test","build",False)["sessions"][0]
            flat=run(root,"test","build",True)["sessions"][0]
            self.assertNotIn("slide",free["required"])
            self.assertNotIn("slide",free["chosen"])
            self.assertIn("slide",flat["chosen"])
            self.assertEqual(free["incomplete"],[retry.name])
            self.assertFalse(flat["missing"])
            self.assertEqual(run(root,"test","other-build",True)["sessions"],[])
    def test_approved_floor_keeps_strict_diagnostic(self):
        r=np.full(100,-.05); p=r+.002
        hit=score_layer(p,r,np.ones(100,dtype=bool))
        self.assertEqual(hit["status"],"pass")
        self.assertEqual(hit["strength"],0.)
        self.assertEqual(hit["strength_at_005_stop_floor"],1.)
        self.assertEqual(score_layer(r+.008,r,np.ones(100,bool))["status"],"pass")
        self.assertEqual(score_layer(r+.011,r,np.ones(100,bool))["status"],"fail")
        self.assertEqual(score_layer(-r,r,np.ones(100,bool))["status"],"fail")

if __name__=="__main__":
    unittest.main()

