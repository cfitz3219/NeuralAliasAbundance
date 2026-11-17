#!/usr/bin/env python3
"""Checks docs/VARIANTS.md against the source tree. See the rules in that file."""
import os, re, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rows, in_register = {}, False
for line in open(os.path.join(root, "docs", "VARIANTS.md"), encoding="utf-8"):
    if line.startswith("## "):
        in_register = line.strip() == "## Register"
        continue
    if not in_register or not line.startswith("|"):
        continue
    cells = [c.strip() for c in line.strip().strip("|").split("|")]
    if cells[0] in ("Name", "") or set(cells[0]) <= set("-"):
        continue
    rows[cells[0]] = cells

errors = []
for name, cells in rows.items():
    if len(cells) != 8 or any(not c for c in cells):
        errors.append(f"row '{name}': needs all 8 fields filled")
        continue
    for proof in re.findall(r"[\w./-]+\.(?:cpp|hpp|py|json|bin)", cells[4]):
        if not os.path.exists(os.path.join(root, proof)):
            errors.append(f"row '{name}': proof file not found: {proof}")

marked = {}
for base in ("src",):
    for folder, _, files in os.walk(os.path.join(root, base)):
        for f in files:
            if not f.endswith((".cpp", ".hpp", ".h", ".inc")):
                continue
            path = os.path.join(folder, f)
            for n, line in enumerate(open(path, encoding="utf-8", errors="ignore"), 1):
                for name in re.findall(r"VARIANT:\s*([\w/.-]+)", line):
                    marked.setdefault(name, f"{os.path.relpath(path, root)}:{n}")
for name, where in marked.items():
    if name not in rows:
        errors.append(f"{where}: VARIANT: {name} has no row in docs/VARIANTS.md")

unmarked = [n for n in rows if n not in marked]
if unmarked:
    print("note: rows with no VARIANT: comment in source yet: " + ", ".join(unmarked))
for e in errors:
    print("error: " + e)
print(f"{len(rows)} rows, {len(marked)} marked in source, {len(errors)} errors")
sys.exit(1 if errors else 0)
