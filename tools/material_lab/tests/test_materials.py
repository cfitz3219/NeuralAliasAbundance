import json
import os
from pathlib import Path
import tempfile
import unittest
import numpy as np
from tools.material_lab import corpus, cache, dds, viewer, render
from tools.material_lab.detail import enhance, mip_chain, Material, down2, linear, srgb


class MaterialMath(unittest.TestCase):
    def setUp(self):
        self.source = corpus.make("wood", 12, 64)

    def test_off_is_exact_and_does_not_alias(self):
        result = enhance(self.source, "wood", 12, 0)
        for a, b in zip(vars(self.source).values(), vars(result).values()):
            np.testing.assert_array_equal(a, b)
            self.assertFalse(np.shares_memory(a, b))

    def test_repeatable_and_seed_changes_detail(self):
        a = enhance(self.source, "wood", 123)
        b = enhance(self.source, "wood", 123)
        np.testing.assert_array_equal(a.normal, b.normal)
        self.assertGreater(np.mean(np.abs(a.normal-enhance(self.source, "wood", 124).normal)), .001)

    def test_protected_lettering_is_exact(self):
        mask = np.ones((64, 64)); mask[12:28, 8:40] = 0
        result = enhance(self.source, "wood", 12, mask=mask)
        for field in ("color", "normal", "roughness"):
            np.testing.assert_array_equal(getattr(result, field)[mask == 0], getattr(self.source, field)[mask == 0])

    def test_finite_normalized_and_low_frequency_colour_retained(self):
        a = enhance(self.source, "wood", 12)
        np.testing.assert_allclose(np.linalg.norm(a.normal, axis=-1), 1, atol=1e-12)
        np.testing.assert_array_equal(a.color[..., 3], self.source.color[..., 3])
        delta = a.color[..., :3]-self.source.color[..., :3]
        for _ in range(4):
            delta = down2(delta)
        self.assertLess(float(np.max(np.abs(delta))), .004)
        self.assertGreater(float(np.mean(np.abs(a.normal-self.source.normal))), .01)

    def test_mips_filter_linear_colour(self):
        color = np.ones((8, 8, 4)); color[::2, :, :3] = 0
        normal = np.zeros((8, 8, 3)); normal[..., 2] = 1
        mips = mip_chain(Material(color, normal, np.ones((8, 8))*.4))
        np.testing.assert_allclose(mips[-1].color[0, 0, :3], .5)
        self.assertAlmostEqual(float(srgb(mips[-1].color[0, 0, 0])), .735356983, places=7)

    def test_unresolved_normals_increase_roughness_once(self):
        a = self.source.copy(); a.normal[:] = [0, 0, 1]; a.roughness[:] = .2
        a.normal[::2, :, :] = [.6, 0, .8]
        a.normal[1::2, :, :] = [-.6, 0, .8]
        mips = mip_chain(a)
        self.assertGreater(float(mips[1].roughness.mean()), .6)
        np.testing.assert_allclose(mips[1].roughness.mean(), mips[-1].roughness.mean())

    def test_flat_normal_does_not_gain_roughness(self):
        a = self.source.copy(); a.normal[:] = [0, 0, 1]; a.roughness[:] = .6
        for mip in mip_chain(a):
            np.testing.assert_allclose(mip.roughness, .6, atol=1e-10)

    def test_rectangular_chain_and_colour_roundtrip(self):
        a = Material(self.source.color[:32], self.source.normal[:32], self.source.roughness[:32])
        self.assertEqual([m.roughness.shape for m in mip_chain(a)], [(32,64),(16,32),(8,16),(4,8),(2,4),(1,2),(1,1)])
        v = np.linspace(0, 1, 1000)
        np.testing.assert_allclose(srgb(linear(v)), v, atol=1e-14)

    def test_invalid_materials_not_silently_interpreted(self):
        with self.assertRaises(ValueError): enhance(self.source, "unknown")
        with self.assertRaises(ValueError): enhance(self.source, "wood", strength=float("nan"))
        bad = self.source.copy(); bad.normal[0,0,0] = float("nan")
        with self.assertRaises(ValueError): enhance(bad, "wood")

    def test_renderer_filters_colour_after_decoding(self):
        pixels = np.array([[[0.,0.,0.,1.], [1.,1.,1.,1.]]])
        sampled = render.sample(pixels, np.array([[[.5,.5]]]))
        np.testing.assert_allclose(sampled[0,0,:3], [.5,.5,.5])

    def test_impossible_extra_mip_rejected(self):
        # A 4x4 texture has only 3 levels, not 4 repeated 1x1 levels.
        raw = dds.header(4,4,4,83,16) + bytes(64)
        with self.assertRaises(ValueError): dds.layout(raw)


@unittest.skipUnless(os.environ.get("NA_TEXCONV"), "set NA_TEXCONV for real BC compression/cache tests")
class CompressedCache(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        corpus.create(cls.root/"sources", 32)
        cls.source = cls.root/"sources"/"oak_boards"
        cls.texconv = os.environ["NA_TEXCONV"]
        cls.entry, cls.record, _ = cache.prepare(cls.source, cls.root/"cache", cls.texconv)

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def test_real_formats_and_complete_mips(self):
        self.assertEqual([m["dxgi_format"] for m in self.record["maps"]], [99, 83, 80])
        for m in self.record["maps"]:
            self.assertEqual(len(m["mips"]), 6)
            self.assertEqual(m["mips"][-1]["width"], 1)
        self.assertEqual(self.record["payload_bytes"], sum((self.entry/m["file"]).stat().st_size-148 for m in self.record["maps"]))

    def test_second_prepare_reuses_exact_files(self):
        before = (self.entry/"normal.dds").stat().st_mtime_ns
        p, r, hit = cache.prepare(self.source, self.root/"cache", self.texconv)
        self.assertTrue(hit); self.assertEqual(p, self.entry); self.assertEqual(r, self.record)
        self.assertEqual(before, (p/"normal.dds").stat().st_mtime_ns)

    def test_packaged_viewer_has_decodable_material_images(self):
        import base64
        import io
        import re
        from PIL import Image
        output = self.root/"Material-Lab.html"
        viewer.write([dict(name="oak_boards", family="wood", split="development",
                           original=str(self.entry), coordinated=str(self.entry))], output)
        html = output.read_text(encoding="utf-8")
        self.assertNotIn("__MATERIAL_DATA__", html)
        encoded = re.search(r'<script id="data" type="application/json">(.*?)</script>', html, re.S)
        data = json.loads(encoded.group(1))
        self.assertEqual(data[0]["name"], "oak_boards")
        for candidate in ("original", "coordinated"):
            self.assertEqual(set(data[0][candidate]["maps"]), {"base_color", "normal", "roughness"})
            for images in data[0][candidate]["maps"].values():
                self.assertEqual(len(images), 6)
                for mip, url in enumerate(images):
                    image = Image.open(io.BytesIO(base64.b64decode(url.split(",",1)[1])))
                    self.assertEqual(image.size, (max(1,32 >> mip),)*2)
                    image.verify()

    def test_truncation_and_format_corruption_rejected(self):
        raw = (self.entry/"normal.dds").read_bytes()
        with self.assertRaises(ValueError): dds.layout(raw[:-1])
        altered = bytearray(raw); altered[128:132] = (99).to_bytes(4, "little")
        self.assertNotEqual(cache.digest(raw), cache.digest(altered))

    def test_manifest_role_or_partial_set_rejected(self):
        import shutil
        dst = self.root/"bad"
        shutil.copytree(self.entry, dst, dirs_exist_ok=True)
        r = json.loads((dst/"manifest.json").read_text()); r["maps"].pop()
        (dst/"manifest.json").write_text(json.dumps(r))
        with self.assertRaises(ValueError): cache.validate(dst)
        r = json.loads((self.entry/"manifest.json").read_text()); r["maps"][0]["channels"][0] = "data"
        (dst/"manifest.json").write_text(json.dumps(r))
        with self.assertRaises(ValueError): cache.validate(dst)

    def test_compressed_normals_retain_direction(self):
        source = corpus.read_maps(self.source)
        expected = enhance(source, "wood", 12).normal
        actual = render.maps(self.entry)["normal"][..., :2]*2-1
        z = np.sqrt(np.maximum(0, 1-np.sum(actual*actual, -1)))
        normal = np.dstack((actual, z))
        angle = np.rad2deg(np.arccos(np.clip(np.sum(normal*expected, -1), -1, 1)))
        # Lossy BC5 + 8-bit source fixture: angular budget, not lossless equality.
        self.assertLess(float(angle.mean()), 2.)
        self.assertLess(float(np.quantile(angle, .99)), 5.)

    def test_source_mutation_changes_cache_identity(self):
        import shutil
        from PIL import Image
        source = self.root/"changed-source"
        shutil.copytree(self.source, source, dirs_exist_ok=True)
        im = Image.open(source/"roughness.png").copy()
        im.putpixel((0,0), 255); im.save(source/"roughness.png")
        entry, r, hit = cache.prepare(source, self.root/"cache", self.texconv)
        self.assertFalse(hit)
        self.assertNotEqual(entry, self.entry)
        self.assertNotEqual(r["entry_key"], self.record["entry_key"])
        cache.validate(self.entry)  # Previously published content remains valid.

    def test_colour_only_baseline_and_interpretation(self):
        import copy
        baseline = copy.deepcopy(self.record)
        baseline["maps"] = baseline["maps"][:1]
        viewer.check_baseline(self.record, baseline)
        for field in ("resource_format", "first_array_slice", "array_size"):
            baseline["maps"][0]["source"].pop(field)
        viewer.check_baseline(self.record, baseline)  # Early v1 records use the same defaults.
        baseline["maps"][0]["source"]["view_format"] = 28
        with self.assertRaises(ValueError): viewer.check_baseline(self.record, baseline)

    def test_source_updated_during_prepare_keeps_decoded_identity(self):
        import shutil
        from unittest.mock import patch
        from PIL import Image
        source = self.root/"concurrent-source"
        shutil.copytree(self.source, source, dirs_exist_ok=True)
        expected_hash = cache.digest((source/"roughness.png").read_bytes())
        def changing_source(folder, source_bytes=None):
            decoded = corpus.read_maps(folder, source_bytes)
            Image.new("L", (32,32), 255).save(source/"roughness.png")
            return decoded
        with patch.object(cache, "read_maps", side_effect=changing_source):
            _, record, _ = cache.prepare(source, self.root/"cache", self.texconv)
        roughness = next(m for m in record["maps"] if m["role"] == "roughness")
        self.assertEqual(roughness["source"]["content_sha256"], expected_hash)
        self.assertNotEqual(expected_hash, cache.digest((source/"roughness.png").read_bytes()))

    def test_previews_change_for_normals_even_without_colour_changes(self):
        original = render.maps(self.entry)
        flat = {**original, "normal": np.full_like(original["normal"], .5)}
        a = np.asarray(render.render(original, size=64, light_phase=.15), dtype=float)
        b = np.asarray(render.render(flat, size=64, light_phase=.15), dtype=float)
        self.assertGreater(float(np.abs(a-b).mean()), 1.)


if __name__ == "__main__": unittest.main()
