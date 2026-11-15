"""Self-contained WebGL comparison using the exact compressed cache mip payloads."""
import base64
import io
import json
from pathlib import Path
from PIL import Image
from . import dds, cache


def source_identity(value):
    return {"resource_format": 0, "first_array_slice": 0, "array_size": 1, **value}


def check_baseline(source_record, baseline_record):
    if source_record["material_id"] != baseline_record["material_id"]:
        raise ValueError("baseline material id mismatch")
    originals = {m["id"]: m for m in source_record["maps"]}
    for m in baseline_record["maps"]:
        original = originals.get(m["id"])
        if not original or any(m[field] != original[field] for field in
                               ("role", "channels", "encoding", "address")) or (
                source_identity(m["source"]) != source_identity(original["source"])):
            raise ValueError("baseline source content or interpretation differs")


def decoded_mips(folder, descriptor):
    raw = (Path(folder)/descriptor["file"]).read_bytes()
    for mip in descriptor["mips"]:
        block = raw[mip["offset"]:mip["offset"]+mip["byte_size"]]
        one = dds.header(mip["width"], mip["height"], 1, descriptor["dxgi_format"], len(block))+block
        yield Image.open(io.BytesIO(one)).convert("RGBA")


def embedded_map(folder, descriptor):
    images = []
    for im in decoded_mips(folder, descriptor):
        buf = io.BytesIO(); im.save(buf, format="PNG")
        images.append("data:image/png;base64,"+base64.b64encode(buf.getvalue()).decode("ascii"))
    return images


def candidate(folder):
    record = cache.validate(folder)
    return dict(key=record["entry_key"], algorithm=record["algorithm"],
                bytes=record["payload_bytes"], fixture=record["fixture"],
                maps={m["role"]: embedded_map(folder, m) for m in record["maps"]})


def write(cases, output):
    # An optional Fable cache manifest can fill the middle panel using identical source inputs.
    payload = []
    for case in cases:
        if case.get("baseline"):
            check_baseline(cache.validate(case["original"]), cache.validate(case["baseline"]))
        row = {k: case[k] for k in ("name", "family", "split")}
        row["original"] = candidate(case["original"])
        row["coordinated"] = candidate(case["coordinated"])
        row["baseline"] = candidate(case["baseline"]) if case.get("baseline") else None
        payload.append(row)
    template = Path(__file__).with_name("viewer.html").read_text(encoding="utf-8")
    encoded = json.dumps(payload, ensure_ascii=True).replace("<", "\\u003c")
    Path(output).write_text(template.replace("__MATERIAL_DATA__", encoded), encoding="utf-8")
