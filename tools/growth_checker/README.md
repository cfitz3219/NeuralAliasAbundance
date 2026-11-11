# Growth checker

A development tool, separate from NeuralAlias. It reads saved evidence (the logs
a run leaves behind, test and replay results) and says, case by case, whether a
build grew, held or receded. It is never loaded by a game and is not part of the
installer. Python 3, standard library only; no network and no model call.

The method is MASTER-PLAN.md section 0. The record formats and the interfaces of
the judgment rules are in `schema.md`.

```
python3 tools/growth_checker ingest <saved run folder> --case <case id> [--expect-build <hash>] [--save]
python3 tools/growth_checker selftest
```

`ingest` reads one saved run. It exits 0 when the evidence was scored, 2 when it
could not be read and 3 when it was refused (wrong build, or no build line where
one was expected). Those codes describe the evidence, never the build.

Data lives in `growth/` at the repository root: cases, scored results and floors.
Raw logs are never committed; a result names its log files by hash.
