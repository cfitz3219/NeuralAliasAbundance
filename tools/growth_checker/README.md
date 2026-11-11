# Growth checker

A development tool, separate from NeuralAlias. It reads saved evidence (the logs
a run leaves behind, test and replay results) and says, case by case, whether a
build grew, held or receded. It is never loaded by a game and is not part of the
installer. Python 3, standard library only; no network and no model call.

The method is MASTER-PLAN.md section 0. The record formats and the interfaces of
the judgment rules are in `schema.md`.

```
python3 tools/growth_checker ingest  <saved run folder> --case <case id> [--expect-build <hash>] [--save]
python3 tools/growth_checker compare --build <hash> [--changed <from>..<to>]
python3 tools/growth_checker report  --build <hash> [--changed <from>..<to>] --out report.html
python3 tools/growth_checker impact  <from>..<to>
python3 tools/growth_checker selftest
```

`ingest` reads one saved run. It exits 0 when the evidence was scored, 2 when it
could not be read and 3 when it was refused (wrong build, or no build line where
one was expected). Those codes describe the evidence, never the build. `compare`
sets every case beside its baseline and floor and exits 1 when a case receded or
is failing.
`report` writes the page for Cort and the proposed ledger row. `impact` sorts
the cases a change can reach into affected, no identified dependency and unknown.

Until the judgment rules exist in `rules/`, the tool answers with the cautious
stand-ins of `defaults.py`: it can say "receded" on a plain fact against the
past, "failing" on a plain fact about one run, and otherwise "not proven". It
never claims growth by itself.

Data lives in `growth/` at the repository root: cases, scored results and floors.
Raw logs are never committed; a result names its log files by hash.
