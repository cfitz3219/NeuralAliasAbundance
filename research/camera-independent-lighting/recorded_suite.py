"""Score recorded lighting trials from one installed build. No live game access."""
import argparse
from collections import defaultdict
import json
from pathlib import Path

from lens_readout import compare, inspect


def ready(directory, identity):
    """A half-written capture or the previous screenshot-only format is ineligible."""
    try:
        for i in range(30):
            prefix = f"{i:05d}-"
            m = json.loads((directory/(prefix+"meta.json")).read_text(encoding="utf-8-sig"))
            if (m["exe"].lower(), m["pid"]) != identity or m["index"] != i:
                return False
            if not m.get("evaluation_succeeded") or m.get("present_pairing") not in ("own_output","overlay_present"):
                return False
            desc = {p["name"]:p for p in m["planes"]}
            for name in ("color","depth","motion","dlssout","present"):
                wanted = desc[name]["bytes"]
                if wanted <= 0 or (directory/(prefix+name+".raw")).stat().st_size != wanted:
                    return False
    except (OSError, KeyError, ValueError, TypeError):
        return False
    return True


def run(root, exe, build, flat):
    required = ["still", "turn1", "turn2"] + (["slide"] if flat else [])
    sessions = defaultdict(lambda: defaultdict(list))
    for directory in sorted(root.glob("j2-lighting-*")):
        try:
            tag = json.loads((directory/"session.json").read_text(encoding="utf-8-sig"))
            condition = tag["kind"].removeprefix("j2-lighting-")
            if condition not in required or tag["exe"].lower()!=exe.lower() or tag["build_commit"]!=build:
                continue
            started = int(tag["process_started_filetime"])
            if started <= 0:
                continue
            sessions[(started, int(tag["pid"]))][condition].append(directory)
        except (OSError, KeyError, ValueError, TypeError):
            continue
    out = []
    for (started,pid), conditions in sorted(sessions.items()):
        picked = {}
        partial = []
        for condition in required:
            for directory in reversed(conditions[condition]):
                if ready(directory,(exe.lower(),pid)):
                    picked[condition]=directory
                    break
                partial.append(directory.name)
        record = dict(pid=pid, process_start=started, required=required,
                      chosen={k:v.name for k,v in picked.items()}, incomplete=partial,
                      missing=[k for k in required if k not in picked],
                      lighting_accuracy="unscored until reference and predicted layers are rendered offline")
        if "still" in picked:
            control = inspect(picked["still"])[3]
            record["still_control"]=control
            observed = control["rejected"].get("insufficient screen motion",0) + control["usable"]
            if observed < 6:
                record["still_declined"] = None
                record["still_why"] = "too few frames with interpretable motion and depth"
            else:
                record["still_declined"] = (control["independent"]["state"]=="no lens yet" and
                                            "rotation_only" not in control["independent"])
        if "turn1" in picked:
            record["turn_test"]=compare(picked["turn1"],slide=picked.get("slide"),repeat=picked.get("turn2"))
        out.append(record)
    return dict(executable=exe, build=build, mode="flat strategy map" if flat else "free look",
                required=required, sessions=out,
                why="recordings inspected" if out else "no labeled sessions found for this game and build")


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root",type=Path,required=True)
    p.add_argument("--exe",required=True)
    p.add_argument("--build",required=True)
    p.add_argument("--flat-map",action="store_true")
    p.add_argument("--output",type=Path,required=True)
    args=p.parse_args()
    result=run(args.root,args.exe,args.build,args.flat_map)
    args.output.write_text(json.dumps(result,indent=2,allow_nan=False)+"\n",encoding="utf8")
    print(f"Lens report: {args.output} ({result['why']}).")

