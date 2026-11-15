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
                model=record["settings"].get("model", ""),
                dimensions={m["role"]: [m["width"], m["height"]] for m in record["maps"]},
                maps={m["role"]: embedded_map(folder, m) for m in record["maps"]})


def attach_models(cases, catalog_path):
    """Resolve prepared alternatives, rejecting duplicate/missing/source-mismatched entries."""
    catalog_path = Path(catalog_path)
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    ids = set()
    result = [{**case, "models": {}} for case in cases]
    for model in catalog:
        key = model["id"]
        if not key or key in ids or key in ("baseline", "conventional"):
            raise ValueError("model ids must be distinct from legacy comparison names")
        ids.add(key)
        index_path = catalog_path.parent/model["index"]
        entries = json.loads(index_path.read_text(encoding="utf-8"))
        for case in result:
            if case["name"] not in entries:
                raise ValueError(f"model {key} has no result for {case['name']}")
            entry = (index_path.parent/entries[case["name"]]).resolve()
            check_baseline(cache.validate(case["original"]), cache.validate(entry))
            case["models"][key] = dict(entry=str(entry), label=model["label"], description=model.get("description", ""))
    return result


def write(cases, output):
    # An optional Fable cache manifest can fill the middle panel using identical source inputs.
    payload = []
    for case in cases:
        for name in ("baseline", "conventional"):
            if case.get(name):
                check_baseline(cache.validate(case["original"]), cache.validate(case[name]))
        row = {k: case[k] for k in ("name", "family", "split")}
        row["original"] = candidate(case["original"])
        row["coordinated"] = candidate(case["coordinated"])
        row['combined'] = bool(case.get('combined'))
        if row['combined']:
            combined = cache.validate(case['coordinated'])
            base = cache.validate(case['baseline'])
            viewer_source = cache.validate(case['original'])
            check_baseline(viewer_source,combined)
            if combined['settings'].get('colour_entry') != base['entry_key']:
                raise ValueError('combined surface does not belong to selected colour model')
            a=next(m['sha256'] for m in combined['maps'] if m['role']=='base_color')
            b=next(m['sha256'] for m in base['maps'] if m['role']=='base_color')
            if a != b:
                raise ValueError('combined comparison changed the colour baseline')
            row['comparisons']={'baseline':{**candidate(case['baseline']), 'label':'PBRify alone',
                                          'description':'PBRify colour with the original surface details.'}}
            # Same colour in both panels: embed once and bind it explicitly in the viewer.
            del row['coordinated']['maps']['base_color']
            payload.append(row)
            continue
        row["baseline"] = candidate(case["baseline"]) if case.get("baseline") else None
        row["conventional"] = candidate(case["conventional"]) if case.get("conventional") else None
        row["comparisons"] = {}
        for key, item in case.get("models", {}).items():
            check_baseline(cache.validate(case["original"]), cache.validate(item["entry"]))
            row["comparisons"][key] = {**candidate(item["entry"]), "label": item["label"],
                                        "description": item["description"]}
        for key, label, description in (("baseline", "Real-ESRGAN · original comparison", "The general-purpose model previously shown in the Lab."),
                                        ("conventional", "Lanczos · conventional upscale", "A conventional upscale with no AI model.")):
            if row[key]:
                row["comparisons"][key] = {**row[key], "label":label, "description":description}
        del row["baseline"], row["conventional"]  # Embed each large candidate once.
        payload.append(row)
    write_payload(payload, output)


def write_payload(payload, output):
    template = Path(__file__).with_name("viewer.html.in").read_text(encoding="utf-8")
    if payload and all(row.get('game_asset') for row in payload):
        template = template.replace('value="65" step="1"><output id="surface-value">65%',
                                    'value="100" step="1"><output id="surface-value">100%')
        template = template.replace('<nav id="materials"', '<a class="badge" href="Material-Lab.html">Back to Material Lab</a>\n<nav id="materials"')
        template = template.replace('<option value="roughness">Roughness only</option>', '')
        template = template.replace('The original colour, surface and roughness maps.',
                                    'The original colour and normal maps from Rome II.')
    encoded = json.dumps(payload, ensure_ascii=True).replace("<", "\\u003c")
    Path(output).write_text(template.replace("__MATERIAL_DATA__", encoded), encoding="utf-8")
