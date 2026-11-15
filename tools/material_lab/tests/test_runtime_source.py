import json
import os
import struct
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import numpy as np
from PIL import Image
from tools.material_baseline import texsub
from tools.material_lab import cache, corpus, dds, runtime_source


def write_dump(folder, header, levels):
    """Same index fields, DX10 header and tight payload as textures.cpp 4525b4e."""
    folder.mkdir(parents=True, exist_ok=True)
    record = dict(zip(runtime_source.FIELDS, header))
    digest = texsub.digest(*header, levels)
    body = b"".join(raw for _, _, raw in levels)
    w, h = max(1, header[2] >> header[4]), max(1, header[3] >> header[4])
    head = bytearray(dds.header(w, h, header[5], header[1], len(levels[0][2])))
    if header[1] in texsub.TEXEL:
        struct.pack_into("<I", head, 8, (struct.unpack_from("<I",head,8)[0] & ~0x80000) | 8)
        struct.pack_into("<I", head, 20, w*texsub.TEXEL[header[1]])
    record.update(identity_domain="texture-subresources-v1", content_sha256=digest,
                  file=f"{digest}.dds", bytes=len(body), slot=3, **{"class":"candidate"})
    (folder/record["file"]).write_bytes(head+body)
    index = folder/"index.jsonl"
    with index.open("a",encoding="utf-8") as f: f.write(json.dumps(record)+"\n")
    return index, digest


def write_capture(folder, header, levels, padding=0):
    folder.mkdir(parents=True, exist_ok=True)
    record = dict(zip(runtime_source.FIELDS, header))
    record.update(identity_domain="texture-subresources-v1", complete_observed_range=True,
                  content_sha256=texsub.digest(*header, levels), levels=[])
    for slice_, mip, raw in levels:
        w, h = max(1, header[2] >> mip), max(1, header[3] >> mip)
        rows = (h+3)//4 if header[1] in texsub.BLOCK else h
        row_bytes = len(raw)//rows
        filename = f"slice-{slice_}-mip-{mip}.bin"
        (folder/filename).write_bytes(b"".join(raw[y*row_bytes:(y+1)*row_bytes]+b"\xa5"*padding for y in range(rows)))
        record["levels"].append(dict(slice=slice_, mip=mip, file=filename, row_pitch=row_bytes+padding))
    path = folder/"capture.json"
    path.write_text(json.dumps(record), encoding="utf-8")
    return path


class RuntimeIdentity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def test_fables_shared_vectors(self):
        levels = [(0, 0, bytes(range(16))), (0, 1, bytes(range(16,20)))]
        vectors = [((28,28,2,2,0,2,0,1), levels, "881dba1cbcd2dae281ca92e0a9c73a8991d0659c5ce5eada825a461706aa4ec1"),
                   ((28,29,2,2,0,2,0,1), levels, "f2daa7e114d47150a6d52aaf820d1af4a2aa4bf939bc5acb773c60fe435b82bc"),
                   ((70,72,8,4,0,1,0,1), levels[:1], "ddb30f684c9a1659f56c259f882ebb8356f44e8951206a6cd1ec848bf2bfa62d"),
                   ((28,28,4,4,1,1,0,1), [(0,1,bytes(range(16)))], "d5a0fbf12418f0569b27f987551ea6883054b8d8eb3570211af6f34ef3cf456f")]
        for index, (header, data, expected) in enumerate(vectors):
            capture = write_capture(self.root/str(index), header, data, padding=13)
            source, top = runtime_source.load_capture(capture)
            self.assertEqual(source["content_sha256"], expected)
            self.assertEqual(top, data[0][2])
        self.assertEqual(texsub.digest(28,28,2,2,0,2,3,2,
            [(s,m,d) for s in (3,4) for _,m,d in levels]),
            "e856d2d87ffba8add0686923abc740517f5145be7b477a31f8a7420e44cdf7d2")

    def test_padding_does_not_change_identity_but_texel_change_does(self):
        capture = write_capture(self.root, (28,28,2,2,0,1,0,1), [(0,0,bytes(range(16)))], padding=4)
        path = self.root/"slice-0-mip-0.bin"
        raw = bytearray(path.read_bytes()); raw[8:12] = b"ABCD"; path.write_bytes(raw)
        source, _ = runtime_source.load_capture(capture)
        self.assertEqual(source["identity_domain"], "texture-subresources-v1")
        raw[0] = 240; path.write_bytes(raw)
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            runtime_source.load_capture(capture)

    def test_partial_range_is_preserved_not_promoted_to_full_texture(self):
        capture = write_capture(self.root, (28,28,16,16,1,1,2,1), [(2,1,bytes(8*8*4))])
        source, raw = runtime_source.load_capture(capture)
        self.assertEqual((source["width"], source["first_mip"], source["mip_count"], source["first_array_slice"]), (16,1,1,2))
        self.assertEqual(runtime_source.decode(source, raw).shape, (8,8,4))

    def test_invalid_capture_layout_rejected(self):
        capture = write_capture(self.root, (28,28,2,2,0,1,0,1), [(0,0,bytes(16))])
        good = json.loads(capture.read_text())
        for changes in ({"mip_count":0}, {"first_mip":32}, {"array_size":2}, {"levels":[]},
                        {"complete_observed_range":False}, {"width":0}):
            capture.write_text(json.dumps({**good, **changes}))
            with self.assertRaises(ValueError): runtime_source.load_capture(capture)

    def test_bgra_and_bgrx_preserve_channels(self):
        for fmt in (87,88,91,93):
            source = dict(width=1, height=1, first_mip=0, view_format=fmt)
            actual = runtime_source.decode(source, bytes([10,20,30,40]))[0,0]
            np.testing.assert_allclose(actual, np.array([30,20,10,255 if fmt in (88,93) else 40])/255)

    def test_fable_dds_dump_is_identical_to_raw_capture(self):
        for header, levels in (((28,29,2,2,0,2,0,1),[(0,0,bytes(range(16))),(0,1,bytes(range(16,20)))]),
                               ((70,72,8,4,0,1,0,1),[(0,0,bytes(range(16)))])):
            index, digest = write_dump(self.root/"dump", header, levels)
            raw = write_capture(self.root/"raw", header, levels, padding=16)
            self.assertEqual(runtime_source.load_dump_entry(index, digest), runtime_source.load_capture(raw))
        path = self.root/"dump"/f"{digest}.dds"
        good = path.read_bytes()
        changed = bytearray(good); changed[-1] ^= 255
        with self.assertRaisesRegex(ValueError,"digest mismatch"):
            path.write_bytes(changed); runtime_source.load_dump_entry(index, digest)
        for bad in (good[:-1], good+b"bad"):
            path.write_bytes(bad)
            with self.assertRaises(ValueError): runtime_source.load_dump_entry(index, digest)

    def test_prepare_runtime_material_keeps_gpu_identity(self):
        texconv = os.environ.get("NA_TEXCONV")
        if not texconv: self.skipTest("set NA_TEXCONV for real BC compression")
        corpus.create(self.root/"fixtures", 32)
        original = self.root/"fixtures"/"oak_boards"
        metadata = json.loads((original/"source.json").read_text())
        metadata["runtime_maps"] = {}
        descriptors = {}
        for role, fmt, packing in (("base_color",29,"rgba_srgb"), ("normal",28,"xyz_unorm_positive_z"),
                                   ("roughness",28,"perceptual_r")):
            raw = Image.open(original/f"{role}.png").convert("RGBA").tobytes()
            capture = write_capture(self.root/role, (fmt,fmt,32,32,0,1,0,1), [(0,0,raw)], padding=8)
            descriptors[role] = json.loads(capture.read_text())
            metadata["runtime_maps"][role] = dict(capture=f"{role}/capture.json", packing=packing)
        (self.root/"source.json").write_text(json.dumps(metadata))
        entry, record, hit = cache.prepare(self.root, self.root/"cache", texconv)
        self.assertFalse(hit)
        for m in record["maps"]:
            self.assertEqual(m["source"]["content_sha256"], descriptors[m["role"]]["content_sha256"])
            self.assertEqual(m["source"]["identity_domain"], "texture-subresources-v1")
        self.assertTrue(cache.prepare(self.root, self.root/"cache", texconv)[2])
        cache.validate(entry)
        # Import the producer's actual DDS/index container: identity/cache key stay the same.
        for role in metadata["runtime_maps"]:
            source, raw = runtime_source.load_capture(self.root/role/"capture.json")
            index, digest = write_dump(self.root/"dump", [source[k] for k in runtime_source.FIELDS], [(0,0,raw)])
            metadata["runtime_maps"][role] = dict(index="dump/index.jsonl", content_sha256=digest,
                                                  packing=metadata["runtime_maps"][role]["packing"])
        (self.root/"source.json").write_text(json.dumps(metadata))
        imported, _, hit = cache.prepare(self.root, self.root/"cache", texconv)
        self.assertEqual(imported, entry)
        self.assertTrue(hit)
        # Mutation after hashing cannot swap the image under the identified bytes.
        decode = runtime_source.decode
        def changing_files(source, raw):
            for role in metadata["runtime_maps"]:
                (self.root/role/"slice-0-mip-0.bin").write_bytes(bytes(136*32))
            return decode(source, raw)
        capture = self.root/"base_color"/"capture.json"
        source, raw = runtime_source.load_capture(capture)
        with patch.object(runtime_source, "decode", changing_files):
            np.testing.assert_array_equal(runtime_source.decode(source, raw), decode(source, raw))
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            runtime_source.load_capture(capture)


if __name__ == "__main__": unittest.main()
