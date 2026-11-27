import unittest
from dataclasses import replace
import numpy as np

from form_lighting import FormReferee
from referee_tuning import AdaptiveForm, SceneReferee, Shape, observe, propose_ao, propose_shape, quality
from test_form_lighting import fixture
from contact_shadow import guard_budget
from tune_replay import held_out_frames, modify


class TuningTests(unittest.TestCase):
    def setUp(self):
        self.rgb,self.q=fixture()
        self.mv=np.zeros((*self.q.shape,2),"f4")
        self.features=observe(self.rgb,self.q,True,self.mv,1/60,20.,(1.3,0.,.1))

    def test_selects_independent_qualified_frames_by_ordinal(self):
        self.assertEqual(held_out_frames([3,5,9,17,24]),[3,9,24])
        self.assertEqual(held_out_frames([17]),[17])
        self.assertEqual(held_out_frames([]),[])
        self.assertEqual(held_out_frames(range(2,29)),[4,13,21])

    def test_quality_boundary_apply_and_recover(self):
        governor=SceneReferee()
        for ms,tier in ((None,3),(19.999,3),(20.,1),(1000./41,1),(25.,1),(25.001,0),(1000./15.01,0),(1000./15,None),(80.,None),(25.,1),(16.,3)):
            pic=replace(self.features,frame_ms=ms)
            proposal=propose_ao(pic,True)
            self.assertEqual(proposal.quality if proposal else None,tier)
            for _ in range(70):
                result=governor.apply(proposal,1/60,picture=pic)
            self.assertEqual(result.quality if result else None,tier)
        self.assertRaises(ValueError,quality,np.nan)


    def test_visual_round_trip_holds_until_source_changes(self):
        governor = SceneReferee()
        first = replace(propose_ao(self.features, True), amount=75, radius=1.)
        bigger = replace(first, amount=90, radius=1.5, denoise=3)
        def feed(option, frames=120, picture=self.features):
            for _ in range(frames):
                result = governor.apply(option, 1/60, picture=picture)
            return result
        feed(first)
        for index in range(240):
            result = feed(bigger if index % 2 else first, 1)
            self.assertEqual(result.amount, 75)
            self.assertEqual(result.radius, 1.)
        self.assertEqual(feed(bigger).amount, 90)
        self.assertEqual(feed(first).amount, 75)
        # Deliberately alternate slowly enough to defeat a cooldown alone.
        for _ in range(4):
            self.assertEqual(feed(bigger).amount, 75)
            self.assertAlmostEqual(feed(first).radius, 1., places=4)
        self.assertEqual(feed(replace(bigger, amount=0), 1).amount, 0)
        changed = replace(self.features, detail=1.-self.features.detail,
                          depth_units=self.features.depth_units*1.5)
        self.assertEqual(feed(bigger, picture=changed).amount, 90)
        self.assertAlmostEqual(feed(bigger, picture=changed).radius, 1.5, places=4)

    def test_own_shadow_response_does_not_keep_restarting_probes(self):
        governor = SceneReferee()
        base = replace(propose_ao(self.features, True), amount=75)
        result = governor.apply(base, .02, picture=self.features)
        submitted = []
        for _ in range(700):
            # The same unchanged source alternately looks under/over target
            # when judged through the output of this simulated strength dial.
            wish = 90 if result.amount < 83 else 75
            result = governor.apply(replace(base, amount=wish), .02, picture=self.features)
            submitted.append(result.amount)
        self.assertGreaterEqual(max(submitted), 83)
        self.assertEqual(set(submitted[-400:]), {75})
        self.assertIsNotNone(governor.targets["amount"].hold_at)
        # An independently supported change to a different dial still works.
        for _ in range(100):
            result = governor.apply(replace(base, amount=90, falloff=.35), .02, picture=self.features)
        self.assertAlmostEqual(result.falloff, .35, places=3)
        self.assertEqual(result.amount, 75)

    def test_fps_cost_feedback_settles_and_recovers(self):
        governor = SceneReferee()
        def frame(ms, reset=False):
            pic = replace(self.features, frame_ms=ms)
            opt = governor.apply(propose_ao(pic, True), ms/1000, reset, picture=pic)
            return opt.quality if opt else -1
        tier = frame(18.)
        transitions = []
        for i in range(900):
            # Switching to Ultra itself costs enough to cross 50 FPS.
            next_tier = frame(20.6 if tier == 3 else 18.2)
            if tier != next_tier:
                transitions.append(next_tier)
            tier = next_tier
        self.assertEqual(transitions, [1, 3, 1])
        for _ in range(100):
            tier = frame(17.)
        self.assertEqual(tier, 3)
        self.assertEqual(frame(1000/15), -1)
        self.assertEqual(frame(20.), -1)
        for _ in range(55):
            tier = frame(20.)
        self.assertEqual(tier, 1)
        self.assertEqual(frame(30.), 0)
        self.assertEqual(frame(16., reset=True), 3)

    def test_noisy_timing_and_missing_inputs(self):
        for boundary, upper, lower in [(20., 3, 1), (25., 1, 0), (1000/15, 0, -1)]:
            governor = SceneReferee()
            tiers = []
            for index in range(360):
                ms = boundary + (.04 if index % 2 else -.04)
                pic = replace(self.features, frame_ms=ms)
                opt = governor.apply(propose_ao(pic, True), ms/1000, picture=pic)
                tiers.append(opt.quality if opt else -1)
            self.assertEqual(tiers[0], upper)
            self.assertEqual(set(tiers[1:]), {lower})
            missing = replace(self.features, depth_units=None)
            self.assertIsNone(governor.apply(None, .02, picture=missing))
            self.assertEqual(governor.apply(propose_ao(self.features, True),.02,
                                            picture=self.features).quality, 1)

    def test_existing_shadow_is_not_an_automatic_strength_penalty(self):
        light=propose_ao(replace(self.features,existing_shadow=0.),False)
        dark=propose_ao(replace(self.features,existing_shadow=1.),False)
        self.assertEqual((light.amount,light.final_power),(dark.amount,dark.final_power))
        y=np.linspace(.04,1,4096).reshape(64,64)
        valid=np.ones_like(y,bool)
        weak=np.full_like(y,-.08)
        retained,_=guard_budget(weak,y,valid,.24,.82,ao_shadow_guard=np.zeros_like(y))
        np.testing.assert_allclose(retained,weak,atol=1e-6)
        heavy=np.where(y<.5,-1.5,0.)
        retained,metrics=guard_budget(heavy,y,valid,.24,.82,ao_shadow_guard=np.zeros_like(y))
        self.assertGreater(metrics["mean_stops"],0.)
        self.assertLessEqual(metrics["mean_stops"],.240001)
        self.assertLessEqual(metrics["worst_tile_stops"],.820001)
        self.assertLessEqual(metrics["contrast_ratio"],1.100001)
        self.assertTrue(np.all(retained[heavy==0]==0.))
        self.assertTrue(np.all(retained<=0.))

    def test_real_thin_geometry_and_shader_sign(self):
        z=self.q.copy()
        z[:,2::5]+=.2
        f=observe(self.rgb,z,True,self.mv,1/60,20.,(1.3,0.,.1))
        self.assertGreater(f.thin,self.features.thin)
        fine=propose_ao(f,True)
        coarse=propose_ao(self.features,True)
        self.assertGreater(fine.thin_occluder,coarse.thin_occluder)
        self.assertGreater(fine.depth_mip_offset,coarse.depth_mip_offset)
        self.assertGreater(fine.sample_distribution,coarse.sample_distribution)

    def test_reset_optional_inputs_and_amount_convergence(self):
        self.assertIsNone(propose_ao(replace(self.features,depth_units=None),True))
        auto=SceneReferee()
        opt=propose_ao(self.features,True)
        auto.apply(opt,1/60)
        for _ in range(120): selected=auto.apply(replace(opt,amount=opt.amount-4),1/60)
        self.assertEqual(selected.amount,opt.amount-4)
        self.assertEqual(auto.apply(replace(opt,temporal=0),1/60).temporal,0.)
        self.assertIsNone(auto.apply(None,1/60))
        bad=observe(self.rgb,self.q,True,np.full_like(self.mv,65504),1/60,20.)
        self.assertIsNone(bad.speed)
        self.assertIsNone(bad.depth_units)

    def test_curvature_applies_all_controls_and_anchor(self):
        p=self.features
        neutral=AdaptiveForm()
        neutral.request=Shape()
        a,b,_=FormReferee().layers(self.rgb,self.q,True,self.mv)
        x,y,_=neutral.layers(self.rgb,self.q,True,self.mv)
        np.testing.assert_allclose(a+b,x+y,atol=1e-6)
        model=AdaptiveForm()
        model.request=propose_shape(p)
        x,y,s=model.layers(self.rgb,self.q,True,self.mv)
        self.assertTrue(np.any(np.abs((x+y)-(a+b))>.005))
        self.assertTrue(.92<=s["contrast"]<=1.10)
        self.assertEqual(s["near_black_before"],s["near_black_after"])
        self.assertEqual(s["controls"]["edge_scale"],model.request.edge)
        self.assertEqual(s["controls"]["guard_scale"],model.request.guard)

    def test_range_limits_and_temporal_contract(self):
        base=propose_ao(self.features,True)
        self.assertGreater(base.temporal,propose_ao(replace(self.features,speed=1.),True).temporal)
        self.assertEqual(propose_ao(self.features,False).temporal,0.)
        out=modify(base,dict(depth_mip_offset=300,thin_occluder=100,amount=3,denoise=-19),True)
        self.assertEqual((out.depth_mip_offset,out.thin_occluder,out.amount,out.denoise),(30.,1.,100,0))


if __name__=="__main__":
    unittest.main()
