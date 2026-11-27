"""Compare two tightly packed RGBA16F generated images against a withheld game frame."""
import argparse
import json
from pathlib import Path

import numpy as np


def box_mean(plane, radius):
    padded = np.pad(plane, radius, mode="edge")
    summed = np.pad(padded, ((1, 0), (1, 0))).cumsum(0, dtype=np.float64).cumsum(1)
    size = 2 * radius + 1
    result = summed[size:, size:] + summed[:-size, :-size] - summed[size:, :-size] - summed[:-size, size:]
    return (result / (size * size)).astype(np.float32)


def read_region(path, width, height, pitch, roi):
    pixels = np.memmap(path, dtype=np.uint8, mode="r")
    if pitch < width * 8 or pixels.size < (height - 1) * pitch + width * 8:
        raise ValueError(f"RGBA16F plane is incomplete in {path}")
    frame = np.ndarray((height, width, 4), dtype="<f2", buffer=pixels, strides=(pitch, 8, 2))
    x, y, right, bottom = roi
    rgb = frame[y:bottom, x:right, :3].astype(np.float32)
    if not np.isfinite(rgb).all():
        raise ValueError(f"Region contains nonfinite pixels in {path}")
    return 255 * np.sign(rgb) * np.abs(rgb) ** (1 / 2.2)


def wire_band(rgb, left, top):
    # Annotation for the November 26 bunker fixture in display pixels.
    xs = [830, 960, 1070, 1180, 1290, 1400, 1510, 1620, 1730, 1840, 1950, 2060, 2170, 2280, 2410]
    ys = [96, 135, 166, 193, 217, 236, 254, 270, 284, 306, 342, 376, 401, 423, 438]
    red, green, blue = np.clip(rgb, 0, 255).transpose(2, 0, 1)
    yellow = (red > 145) & (green > 111) & (blue < .72 * green) & (red > 1.035 * green)
    row = np.arange(rgb.shape[0])[:, None]
    col = np.arange(rgb.shape[1])
    predicted = np.interp(left + col, xs, ys) - top
    points = np.where(yellow & (np.abs(row - predicted) < 14), row, np.nan)
    known = np.isfinite(points).any(axis=0)
    if not known.any():
        raise ValueError("No cable was found in the annotated region")
    axis = np.interp(col, col[known], np.nanmedian(points[:, known], axis=0))
    distance = np.abs(row - axis)
    return (distance >= 32) & (distance <= 96)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("truth_index", type=int)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--roi", nargs=4, type=int, metavar=("X", "Y", "RIGHT", "BOTTOM"))
    parser.add_argument("--bunker-wire-band", action="store_true")
    args = parser.parse_args()
    meta = [json.loads((args.capture / f"{i:05d}-meta.json").read_text()) for i in range(args.truth_index - 1, args.truth_index + 2)]
    ordinals = [m["present_ordinal"] for m in meta]
    if ordinals != list(range(ordinals[0], ordinals[0] + 3)):
        raise ValueError(f"The three real frames must be consecutive, found {ordinals}")
    plane = next(p for p in meta[1]["planes"] if p["name"] == "present")
    width, height = plane["width"], plane["height"]
    if plane["dxgi_format"] != 10:
        raise ValueError("This measurement requires linear RGBA16F colour")
    roi = args.roi or [0, 0, width, height]
    if args.bunker_wire_band:
        if (width, height) != (3440, 1440):
            raise ValueError("Cable annotation is for the 3440 by 1440 bunker fixture")
        roi = [830, 48, 2410, 560]
    if not (0 <= roi[0] < roi[2] <= width and 0 <= roi[1] < roi[3] <= height):
        raise ValueError("Region falls outside the image")
    truth_path = args.capture / f"{args.truth_index:05d}-present.raw"
    truth = read_region(truth_path, width, height, plane["row_pitch"], roi)
    mask = wire_band(truth, *roi[:2]) if args.bunker_wire_band else np.ones(truth.shape[:2], dtype=bool)
    luma = np.array([.2126, .7152, .0722], dtype=np.float32)
    ref = truth @ luma
    ref_detail = np.abs(ref - box_mean(ref, 2))[mask].mean()
    result = {"ordinals": ordinals, "pixels": int(mask.sum())}
    for name, path in (("before", args.before), ("after", args.after)):
        generated = read_region(path, width, height, width * 8, roi)
        delta = generated - truth
        lum = generated @ luma
        result[name] = {
            "broad_luma_error": float(np.abs(box_mean(delta @ luma, 4))[mask].mean()),
            "mean_abs_channel_error": float(np.abs(delta[mask]).mean()),
            "over_12_pct": float(np.mean(np.abs(delta).max(axis=2)[mask] > 12) * 100),
            "detail_ratio": float(np.abs(lum - box_mean(lum, 2))[mask].mean() / ref_detail) if ref_detail else None,
        }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
