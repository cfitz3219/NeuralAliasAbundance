import argparse
import json
from pathlib import Path
import time
from . import corpus, cache, viewer, render


def main():
    parser = argparse.ArgumentParser(description="Offline NeuralAlias material preparation")
    sub = parser.add_subparsers(dest="command", required=True)
    demo = sub.add_parser("demo", help="generate six synthetic inputs, compressed material sets and comparison")
    demo.add_argument("--output", type=Path, required=True)
    demo.add_argument("--texconv", type=Path, required=True)
    demo.add_argument("--size", type=int, default=256)
    demo.add_argument("--strength", type=float, default=1.)
    demo.add_argument("--baseline-index", type=Path, help="JSON object mapping material ids to Fable's cache directories")
    prepare = sub.add_parser("prepare", help="prepare a source.json + PNG material folder")
    prepare.add_argument("source", type=Path)
    prepare.add_argument("--cache", type=Path, required=True)
    prepare.add_argument("--texconv", type=Path, required=True)
    prepare.add_argument("--strength", type=float, default=1.)
    valid = sub.add_parser("validate"); valid.add_argument("entry", type=Path)
    args = parser.parse_args()
    if args.command == "validate":
        record = cache.validate(args.entry)
        print(json.dumps(dict(key=record["entry_key"], maps=len(record["maps"]), payload_bytes=record["payload_bytes"])))
    elif args.command == "prepare":
        p, _, hit = cache.prepare(args.source, args.cache, args.texconv, args.strength)
        print(json.dumps(dict(entry=str(p), reused=hit)))
    else:
        args.output.mkdir(parents=True, exist_ok=True)
        cases = corpus.create(args.output/"sources", args.size)
        external = json.loads(args.baseline_index.read_text()) if args.baseline_index else {}
        index = []; start = time.perf_counter()
        for c in cases:
            src = args.output/"sources"/c["material_id"]
            old, _, _ = cache.prepare(src, args.output/"cache", args.texconv, candidate="original")
            new, r, hit = cache.prepare(src, args.output/"cache", args.texconv, args.strength)
            row = dict(name=c["material_id"], family=c["family"], split=c["split"],
                       original=str(old.resolve()), coordinated=str(new.resolve()))
            if c["material_id"] in external:
                base = Path(external[c["material_id"]]).resolve()
                rb = cache.validate(base)
                viewer.check_baseline(r, rb)
                row["baseline"] = str(base)
            index.append(row)
            print(f"{c['material_id']}: {r['payload_bytes']} bytes, cache {'hit' if hit else 'prepared'}", flush=True)
        viewer.write(index, args.output/"Material-Lab.html")
        render.previews(index, args.output)
        (args.output/"comparison.json").write_text(json.dumps(index, indent=2)+"\n")
        print(json.dumps(dict(viewer=str((args.output/"Material-Lab.html").resolve()), seconds=time.perf_counter()-start)))


if __name__ == "__main__": main()
