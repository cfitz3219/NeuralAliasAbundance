"""Package the original-baseline placement/flow build. Never deploy to a game."""
import argparse
import json
import re
import shutil
from pathlib import Path
from prepare_se5_flow_repair import digest, publish, validate


def prepare(template, dll, documents, source, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("An exact source commit is required")
    baseline = validate(template)
    if baseline["source_commit"] != "6f6b806ccb7b276a65410e4952f0c34ee5911aec":
        raise ValueError("Use the preserved original baseline transaction")
    script = (template / "deploy.ps1").read_text(encoding="utf-8-sig")
    script, count = re.subn(r"(?m)^\$TestCommit\s*=.*$", "$TestCommit = '" + commit + "'", script)
    if count != 1:
        raise ValueError("Missing transaction source identifier")
    # Reuse the transaction, receipt, Results and rollback. The original-reset
    # package deliberately reset choices; subsequent updates preserve them again.
    start = script.index("    # Cort requests the original baseline,")
    end = script.index("    $commit = $TestCommit", start)
    script = script[:start] + script[end:]
    script = script.replace("mip_bias = '-1.0' }", "mip_bias = '-1.0'; flow_loosen = '50' }")
    script = script.replace("foreach ($k in 'sharpness', 'preset', 'mip_bias')", "foreach ($k in 'sharpness', 'preset', 'mip_bias', 'flow_loosen')")
    script = script.replace("flow=1\n", "flow=1\nflow_loosen=$($Keep['flow_loosen'])\n")
    script = script.replace("foreach ($p in '^j2 on=',", "foreach ($p in '^j2_entry ', '^j2 on=',")
    script = script.replace("AMD moving-object flow: accepted", "AMD residual corrections: accepted")
    script = script.replace("Home > Moving-object motion (test)", "Home > Enhancements > DLAA enhancements > Moving-object motion (test)")
    script = script.replace("flicker-diagnostic", "post-entry and flow").replace("flicker diagnostic", "post-entry and flow")
    # Header describes this increment; the historical original remains intact.
    script = ("# Original6f transaction, completed post-entry repair + Claude flow/UI676368f.\n"
              "# Additive installation only; previous logs/settings retained; rollback uses the existing receipt.\n"
              + script[script.index("param("):])
    if "flow_loosen=$($Keep['flow_loosen'])" not in script:
        raise ValueError("Missing combined flow setting")
    package = documents / ("NA-SE5-camfix-" + commit[:7])
    package.mkdir()
    for item in baseline["payload"]:
        shutil.copy2(template / item["name"], package / item["name"])
    (package / "deploy.ps1").write_text(script, encoding="utf-8-sig")
    shutil.copy2(dll, package / "NeuralAliasDXGI.dll")
    shutil.copy2(source / "docs/SE5-POST-ENTRY-REPAIR.md", package / "ENGINEERING.md")
    (package / "README.txt").write_text(
        "NeuralAlias SE5 camfix " + commit + "\n"
        "Original6f + actual post-entry placement + Claude flow/UI676368f.\n"
        "Close SE5, run Documents\\NA-se5-camfix-update.cmd, then use the existing NA-se5-play.cmd.\n"
        "Home > Enhancements > DLAA enhancements: moving-object motion, rules, jitter, texture detail.\n"
        "Flow starts ON. Moving-object rules defaults to 50%; 0% restores the original strict gates.\n"
        "Retains saved sharpness/preset/mip-bias/rules. Camera jitter behavior remains the original baseline.\n"
        "Placement learns for a few frames; j2_entry in saved results records actual-operation matches.\n"
        "F8 saves evaluation/final-image captures. After exiting, run NA-se5-camfix-results.cmd.\n"
        "NA-se5-camfix-rollback.cmd removes this additive installation using its receipt.\n"
        "Original/rejected packages and launcher backups remain available. Native files are never replaced.\n"
        "Automated regression evidence; actual game picture quality/flicker remains Cort's test.\n",
        encoding="utf-8")
    (package / "RESTORE-NOTES.txt").write_text(
        "The original baseline package remains at NA-SE5-camfix-original-6f6b806.\n"
        "Run its deploy.ps1 -Action Deploy with the game closed to return to exact6f.\n"
        "The usual rollback command restores the installation's original files, rather than selecting a prior build.\n",
        encoding="utf-8")
    manifest = {"source_commit": commit, "base_commit": baseline["source_commit"],
        "claude_flow_commit": "676368fc9d5850ac0f552f4353c8ecd7ec1ff4a5",
        "payload": [{"name": p.name, "sha256": digest(p)} for p in sorted(package.iterdir())
                    if p.is_file() and p.name != "package.json"]}
    (package / "package.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    validate(package, commit)
    return package


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("template", "dll", "documents", "source", "commit"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--publish", action="store_true")
    args = parser.parse_args()
    package = prepare(Path(args.template), Path(args.dll), Path(args.documents), Path(args.source), args.commit)
    print(package)
    if args.publish:
        print(publish(Path(args.documents), package, args.commit, title="post-entry + flow"))
