"""A3 analytical invariants and adversarial candidate checks; no GPU required."""
import copy
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from scene import project, render, unproject, write_frame
from check import compare


class FixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.frames = [render(f, 64, 48) for f in range(12)]

    def test_static_camera_motion_independent_formula(self):
        frame = self.frames[3]
        fx, fy = frame['camera']['focal']
        count = 0
        for i, ident in enumerate(frame['planes']['surface_id']):
            if ident == 1 and frame['planes']['history_valid'][i]:
                # Translation of camera +0.12,+0.03 at fixed Z=12;
                # current-to-previous x positive, screen y negative.
                mx, my = frame['planes']['motion'][i]
                self.assertAlmostEqual(mx, fx*0.12/12)
                self.assertAlmostEqual(my, -fy*0.03/12)
                count += 1
        self.assertGreater(count, 100)

    def test_independent_object_velocity(self):
        frame = self.frames[3]
        count = 0
        for i, ident in enumerate(frame['planes']['surface_id']):
            if ident == 2 and frame['planes']['history_valid'][i]:
                mx, my = frame['planes']['motion'][i]
                self.assertAlmostEqual(mx, frame['camera']['focal'][0]*(0.12-0.18)/5)
                self.assertAlmostEqual(my, -frame['camera']['focal'][1]*0.03/5)
                count += 1
        self.assertGreater(count, 0)

    def test_animation_has_nonrigid_motion(self):
        frame = self.frames[3]
        values = []
        c = frame['camera']
        shear_delta = 0.45*(math.sin(2*0.6)-math.sin(3*0.6))
        for i, ident in enumerate(frame['planes']['surface_id']):
            if ident != 3 or not frame['planes']['history_valid'][i]:
                continue
            y = i//frame['width']
            local_y = c['position'][1] - (y+0.5-c['jitter'][1]-frame['height']/2)*3.5/c['focal'][1] + 0.2
            expected = c['focal'][0]/3.5*(0.12+shear_delta*local_y)
            value = frame['planes']['motion'][i][0]
            self.assertAlmostEqual(value, expected)
            values.append(value)
        self.assertGreater(max(values)-min(values), 0.1)

    def test_projective_depth_and_matrices(self):
        frame = self.frames[3]
        c = frame['camera']
        for i, ident in enumerate(frame['planes']['surface_id']):
            z = {1: 12.0, 2: 5.0, 3: 3.5}[ident]
            d = frame['planes']['depth'][i]
            recovered_z = c['near']*c['far']/(c['far']-d*(c['far']-c['near']))
            self.assertAlmostEqual(z, recovered_z)
        world = [1.25, -0.2, 5, 1]
        mul = lambda m, v: [sum(m[r*4+k]*v[k] for k in range(4)) for r in range(4)]
        clip = mul(c['projection'], mul(c['view'], world))
        px, py = project(c, world[:3])
        self.assertAlmostEqual(px, (clip[0]/clip[3]+1)*frame['width']/2)
        self.assertAlmostEqual(py, (1-clip[1]/clip[3])*frame['height']/2)
        for actual, expected in zip(unproject(c, (px, py), 5), world[:3]):
            self.assertAlmostEqual(actual, expected)

    def test_cut_and_disocclusion(self):
        for index in (0, 6):
            frame = self.frames[index]
            self.assertTrue(frame['reset'])
            self.assertFalse(any(frame['planes']['history_valid']))
            self.assertFalse(any(frame['planes']['transparent_history_valid']))
            self.assertTrue(all(v == [0.0, 0.0] for v in frame['planes']['motion']))
        self.assertGreater(sum(sum(f['planes']['disocclusion']) for f in self.frames), 0)
        self.assertTrue(any(self.frames[7]['planes']['history_valid']))

    def test_transparency_and_hud_separate_truth(self):
        frame = self.frames[3]
        p = frame['planes']
        mixed = 0
        for i, alpha in enumerate(p['transparent_alpha']):
            if alpha and p['history_valid'][i] and p['transparent_history_valid'][i]:
                mixed += p['motion'][i] != p['transparent_motion'][i]
            if p['hud'][i]:
                self.assertEqual(p['composite_color'][i], [1.0]*3)
                self.assertNotEqual(p['scene_color'][i], p['composite_color'][i])
            else:
                self.assertEqual(p['scene_color'][i], p['composite_color'][i])
        self.assertGreater(mixed, 0)

    def test_jitter_not_motion_and_odd_extent(self):
        a, b = self.frames[2], self.frames[3]
        self.assertNotEqual(a['camera']['jitter'], b['camera']['jitter'])
        odd = render(3, 65, 49)
        self.assertEqual(len(odd['planes']['motion']), 65*49)
        self.assertTrue(compare(odd, odd)['passed'])
        self.assertEqual(render(3, 65, 49), odd)

    def test_checker_rejects_wrong_units_depth_and_masks(self):
        truth = self.frames[3]
        for name in ('motion', 'depth', 'history_valid', 'scene_color', 'transparent_motion', 'camera'):
            candidate = copy.deepcopy(truth)
            if name in ('motion', 'transparent_motion'):
                candidate['planes'][name] = [[-x*2, -y*2] for x, y in candidate['planes'][name]]
            elif name == 'camera':
                candidate['camera']['jitter'][0] += 0.25
            elif name == 'history_valid':
                candidate['planes'][name] = [False]*len(candidate['planes'][name])
            elif name == 'scene_color':
                candidate['planes'][name] = candidate['planes']['composite_color']
            else:
                candidate['planes'][name] = [1-d for d in candidate['planes'][name]]
            self.assertFalse(compare(truth, candidate)['passed'], name)

    def test_malformed_or_mismatched_candidates(self):
        truth = self.frames[3]
        for mutation in ('nan', 'length', 'dimensions', 'frame', 'convention'):
            candidate = copy.deepcopy(truth)
            if mutation == 'nan': candidate['planes']['depth'][0] = math.nan
            if mutation == 'length': candidate['planes']['motion'].pop()
            if mutation == 'dimensions': candidate['width'] = 0
            if mutation == 'frame': candidate['frame'] = 4
            if mutation == 'convention': candidate['conventions']['motion'] = 'UV'
            with self.assertRaises(ValueError): compare(truth, candidate)

    def test_export_and_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            truth = self.frames[3]
            write_frame(root, truth)
            path = root/'frame-003.json'
            saved = json.loads(path.read_text(encoding='utf-8'))
            self.assertEqual(saved, truth)
            image = (root/'frame-003-scene_color.ppm').read_bytes()
            self.assertEqual(len(image.split(b'\n', 3)[3]), 64*48*3)
            candidate = root/'candidate.json'
            candidate.write_text(json.dumps(saved), encoding='utf-8')
            command = [sys.executable, str(Path(__file__).with_name('check.py')),
                       '--truth', str(path), '--candidate', str(candidate)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            saved['planes']['depth'][0] = 0.0
            candidate.write_text(json.dumps(saved), encoding='utf-8')
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
            candidate.write_text('{}', encoding='utf-8')
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)


if __name__ == '__main__':
    unittest.main()
