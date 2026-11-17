"""Offline PyTorch model adapter; reuses Fable's baseline cache/mip/compressor.

Run: python -m tools.material_lab.model_runner --help
Model weights and GPU dependencies stay outside the runtime and source tree.
"""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import time
import numpy as np
from tools.material_baseline.__main__ import PAD, prepare


class ModelUpscale:
    def __init__(self, weights, device="cuda"):
        import torch
        from spandrel import ImageModelDescriptor, ModelLoader
        self.torch = torch
        self.device = device
        # Official source weights only; restrict pickle deserialization to weights.
        state = torch.load(weights, map_location="cpu", weights_only=True)
        self.model = ModelLoader().load_from_state_dict(state)
        if not isinstance(self.model, ImageModelDescriptor) or self.model.input_channels != 3 or self.model.output_channels != 3:
            raise ValueError("expected a three-channel image upscaler")
        self.model.to(device).eval()
        self.scale = self.model.scale
        self.architecture = self.model.architecture.id

    def __call__(self, rgb, scale, _upscaler, _model, _work):
        if scale != self.scale:
            raise ValueError(f"model has fixed {self.scale}x scale; requested {scale}x")
        torch = self.torch
        h, w = rgb.shape[:2]
        padded = np.pad(rgb, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="wrap")
        tensor = torch.from_numpy(padded.astype(np.float32)/255).permute(2,0,1).unsqueeze(0).to(self.device)
        with torch.inference_mode():
            output = self.model(tensor)
        expected = (1,3,(h+2*PAD)*scale,(w+2*PAD)*scale)
        if tuple(output.shape) != expected or not torch.isfinite(output).all().item():
            raise ValueError("model produced invalid pixels or unexpected dimensions")
        pixels = output[0,:,PAD*scale:(PAD+h)*scale,PAD*scale:(PAD+w)*scale]
        # Existing baseline contract is sRGB uint8; identical downstream conversion.
        return pixels.permute(1,2,0).clamp(0,1).mul(255).round().to(torch.uint8).cpu().numpy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    parser.add_argument("--texconv", type=Path, required=True)
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--model-id", required=True)
    parser.add_argument("--device", choices=("cuda","cpu"), default="cuda")
    args = parser.parse_args()
    runner = ModelUpscale(args.weights, args.device)
    identity = dict(algorithm="spandrel-colour-upscale", version="1",
                    upscaler_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    model_sha256=hashlib.sha256(args.weights.read_bytes()).hexdigest(),
                    inference=dict(torch=runner.torch.__version__, spandrel=importlib.metadata.version("spandrel"),
                                   architecture=runner.architecture, precision="float32", device=args.device,
                                   padding=PAD, tiling="whole-image"))
    index, timings = {}, []
    print(json.dumps(dict(model=args.model_id, scale=runner.scale, **identity)),flush=True)
    for folder in sorted(p for p in args.sources.iterdir() if (p/"source.json").is_file()):
        start = time.perf_counter()
        entry, record, hit = prepare(folder, args.cache, args.texconv, "spandrel", args.model_id,
                                     runner.scale, identity, upscale=runner)
        index[record["material_id"]] = str(entry.resolve())
        timings.append(dict(material=record["material_id"], seconds=round(time.perf_counter()-start,3), reused=hit))
        print(json.dumps(timings[-1]),flush=True)
    args.index.parent.mkdir(parents=True,exist_ok=True)
    args.index.write_text(json.dumps(index,indent=2)+"\n",encoding="utf-8")
    args.index.with_suffix(".receipt.json").write_text(json.dumps(dict(identity=identity, materials=timings),indent=2)+"\n",encoding="utf-8")


if __name__ == "__main__": main()
