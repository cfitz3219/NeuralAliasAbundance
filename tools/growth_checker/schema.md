# Growth checker: record formats and rule interfaces (schema 1)

The growth checker is a development tool, separate from NeuralAlias (MASTER-PLAN.md
section 0). This file is the contract between its parts. Every record is JSON. The
judgment rules in `rules/` are pure functions over these records: no file access,
no network, no clock.

A complete record produced by the tool itself is in `samples/`, and a real one is
`growth/results/fc0e6bdd/wwii-campaign-dlaa/`. If a rule needs a field that is not
here, ask for it in COMMS; do not work around it.

## 1. Where things live

| Path | Content |
|---|---|
| `growth/cases/<case>.json` | One case: a game on a route with its settings, scene and budget |
| `growth/results/<build8>/<case>/<evidence_id>@<scorer version>.json` | One scored result. A newer scorer writes a new file beside the old one. |
| `growth/floors/<case>.json` | The floor of a case, its history and any open proposal |
| `growth/dependencies.json` | Source area, capability or provider, cases (for the impact list) |
| `docs/BUILD-LEDGER.md` | One row per baseline |

Raw logs are never committed. A result names its log files by SHA-256.

## 2. Case (`growth.case/1`)

```json
{
  "schema": "growth.case/1",
  "id": "wwii-campaign-dlaa",
  "game": "Call of Duty WWII (campaign)",
  "profile": "wwii",
  "route": "B",
  "api": "dx11",
  "provider": "native11",
  "engine": {"name": "IW engine (Sledgehammer)", "source": "declared from public knowledge; the program does not observe it"},
  "settings": {"mode": "DLAA"},
  "scene": {"id": null, "description": "..."},
  "budget": {"fps_min": 40, "longest_gap_seconds_max": null, "set_by": "Cort, 2026-11-09 ..."}
}
```

`profile` is the installer's profile id. `provider` is one of `native11`, `dx12`,
`route_a`. `scene.id` stays null until Cort fixes a standard scene for the case.
Only Cort changes a budget.

## 3. Evidence manifest (`growth.manifest/1`)

What a piece of evidence is and where it came from. One manifest per status log.

| Field | Meaning |
|---|---|
| `evidence_id` | `<build8 or "nobuild">-<first 12 hex of the status log's SHA-256>`. The same evidence keeps its id however often it is scored. |
| `evidence_type` | `live_run` (Cort's PC, a real game), `replay` (recorded inputs through the current rules), `fixture_test`, `synthetic` |
| `producer.build` | The commit that wrote the log, or null. **Read from the evidence, never from a caller's label.** |
| `producer.build_source` | Strongest first: `status_header` (the log's own build line), `startup_header`, `run_manifest`, `results_summary` (the text file the saving script wrote), `unknown` |
| `session` | `id` (the log's process suffix), `first_sample`, `last_sample` (clock times; the log carries no date), `saved_folder`, `saved_at`, `profile_hint` (from the folder name) |
| `case`, `case_source` | The case this evidence was filed under and who said so (`ingest argument`, later `run_manifest`) |
| `files[]` | `name`, `role` (`status`, `startup`, `observe`, `saved-with-run`), `bytes`, `sha256` |
| `scorer` | `tool`, `version`, `schema` |
| `notes[]` | Sentences about provenance (a weak build source, a case that does not match the folder's profile, mixed builds) |

Scoring an old log again with a newer scorer is an analysis update: same
`evidence_id`, new `scorer.version`. It is never a new result of NeuralAlias.

## 4. Result (`growth.result/1`)

```json
{
  "schema": "growth.result/1",
  "manifest": { },
  "status": "scored",
  "status_reason": null,
  "provider": "native11", "api": "dx11", "route": "B",
  "sessions": [ ],
  "measurements": { },
  "observed": { },
  "jobs": { },
  "owner_verdicts": [ ],
  "patterns": { },
  "dependencies_in_use": [ ]
}
```

`status` is the outcome of *reading*, kept apart from any verdict on the build:
`scored`; `unreadable` (no status log, no samples, no known counters);
`refused` (an expected build was named and the evidence is from another build or
carries none). Unreadable and refused records have no measurements.

### 4.1 Sessions

A log is split wherever a counter that only counts up goes down (the game or the
program restarted inside one file). Counters are never subtracted across a split.
Each entry: `first_sample`, `last_sample`, `samples`, `presented`, `scene`,
`delivered`, `run_seconds`, `unlogged_seconds`, `play_seconds`,
`delivering_seconds`, `longest_gap_seconds`, `longest_gap_window_seconds`,
`longest_gap_started`, `first_delivery_after_seconds`. Run-level measurements
sum the sessions; the gap is the worst session's.

### 4.2 Measurements

Every measurement is one of:

```json
{"state": "measured", "value": 96.34, "unit": "percent", "basis": "derived", "limits": "..."}
{"state": "unknown", "value": null, "why": "the log has no jitter counter"}
{"state": "not_applicable", "value": null, "why": "..."}
```

**A missing value is `unknown`. It is never zero and can never satisfy a limit.**
`basis`: `counter` (the program counted it), `sampled` (estimated from the
periodic status samples, only as fine as `sample_period_seconds`), `event`,
`derived`, `observed`. `limits` is a sentence the report prints beside the number.

| Name | Unit | Basis | Notes |
|---|---|---|---|
| `frames_presented` | frames | counter | native DX11 `native11 frames=`, DX12 `j2_coverage presents=` |
| `frames_scene` | frames | counter | Frames the program judged to be a finished scene. Native DX11 `fired=`. **Unknown on DX12**: not logged apart from presented. |
| `frames_delivered` | frames | counter | Frames with DLSS output delivered, by the program's count |
| `scene_share_of_presented` | percent | derived | Makes a drop in recognised scenes visible. Cannot tell menus from missed gameplay. |
| `delivered_share_of_scene` | percent | derived | The old scorer's `dlaa_percent` for native DX11 |
| `delivered_share_of_presented` | percent | derived | |
| `run_seconds`, `unlogged_seconds`, `play_seconds` | seconds | sampled | Play = samples with delivery opportunities (scene frames, or presented frames where scene is not logged). Unlogged = holes over six sample periods. |
| `sample_period_seconds` | seconds | sampled | |
| `longest_gap_seconds`, `longest_gap_window_seconds`, `longest_gap_started` | seconds, clock | sampled | A gap over observed opportunities. Intended Off, menu and loading are not told apart from unexpected misses. |
| `not_delivering_share_of_play` | percent | sampled | Share of play time in samples where under 98% of opportunities got DLSS |
| `time_to_first_delivery_seconds` | seconds | sampled | |
| `fps_play_median`, `fps_play_lowest`, `frame_ms_play_median` | fps, ms | sampled | Over **every** play sample, delivering or not |
| `fps_median_while_delivering`, `fps_lowest_while_delivering` | fps | sampled | Conditional: only samples at 98% delivery or more. Read beside `not_delivering_share_of_play`. |
| `frames_jittered` | frames | counter | |
| `menu_draws` | draws | counter | |
| `camera_found_by` | frames per rule | counter | A dict. Native DX11 only so far. |
| `missed_because` | frames per reason | counter | A dict. Unknown when the log gives no reasons (a telemetry gap). |

`observed` holds settings the log states once DLSS is delivering, for example
`{"sharpness": {"last": 100, "distinct": [13, 100]}, "relative_layout": "seed"}`.

### 4.3 Jobs

`jobs[<id>] = {"state", "note", "evidence": [measurement names], "source"?}`.
States: `verified`, `failed`, `untested`, `not_applicable`, `unknown`.

| Route | Job ids |
|---|---|
| Both | `home_menu_opens`, `home_controls_take_effect`, `status_truthful`, `performance_budget`, `presentation_modes`, `no_crash_or_hang` |
| A | `mode_changes_render_size`, `features_do_what_status_claims` |
| B | `api_chosen`, `camera_found`, `depth_found`, `motion_found`, `jitter_applied`, `dlaa_delivered`, `dlaa_reaches_picture`, `continuity`, `cost` |

A state from a log says what the program counted. A claim about the picture
(`dlaa_reaches_picture`) needs the owner's verdict:
`owner_verdicts[] = {"job", "state", "by", "said"}`; a job set that way carries
`"source": "owner_verdict"`.

### 4.4 Patterns and dependencies

`patterns[<name>]` is `{"state": "observed", "value", "source"}`,
`{"state": "unknown", "why"}` or `{"state": "not_applicable", "why"}`. Names:
`api`, `provider`, `camera_admission` (a dict of method to frames),
`camera_layout_from` (`seed`, `learned`, kept apart from the admission method),
`depth_convention`, `jitter_route`, `motion_source`, `hud_composition`,
`presentation`. Most are unknown until NeuralAlias logs them (task G9).

`dependencies_in_use[] = {"variant", "kind", "frames", "source"}` with `variant`
a name from `docs/VARIANTS.md` and `kind` one of `shared_rule`,
`shared_rule_with_adapter_hint`, `title_specific`, `provenance_unknown`.

## 5. Floor (`growth.floor/1`)

```json
{
  "schema": "growth.floor/1",
  "case": "wwii-campaign-dlaa",
  "floor": {"delivered_share_of_scene": {"kind": "min", "value": 96.0}, "longest_gap_seconds": {"kind": "max", "value": 8.0}},
  "set_by": {"evidence_id": "...", "build": "...", "decided": "..."},
  "proposal": null,
  "history": [ ]
}
```

A better run writes a `proposal` (`{"floor", "from": evidence_id, "awaits"}`); the
floor itself changes only when rule A1 confirms it from the case's next planned
run, or by Cort's decision. Replaced floors move to `history`.

## 6. Rule interfaces (Astra, tasks A1 to A4)

Each module lives in `rules/`, imports nothing but the standard library, and is
called with records exactly as above. `tests/vectors/<rule>/*.json` hold
`{"about", "call", "args", "expect"}`; the test runner feeds `args` to the function
named in `call` and compares with `expect`.

```python
# rules/comparison.py  (A1)
def judge(baseline, candidate, case):
    """Two result records of one case ->
    {"verdict": "comparable" | "comparable_with_limits" | "not_comparable",
     "limits": [sentence, ...], "reasons": [sentence, ...],
     "tolerance": {measurement name: {"absolute": x, "relative_percent": y}}}"""

def floor_action(case, floor, candidate, comparison, earlier_proposal):
    """-> {"action": "keep" | "propose" | "confirm" | "needs_owner", "floor": {...} or None, "reason": sentence}"""

# rules/guards.py  (A2)
def check(baseline, candidate, case):
    """baseline may be None -> [{"guard": id, "severity": "blocks" | "warns",
     "message": sentence the report prints, "fields": [measurement names]}]"""

# rules/transfer.py  (A3)
def credit(results_by_case, cases):
    """{case id: [result, ...]}, {case id: case} ->
    {"credits": [{"kind", "case", "pattern", "because"}],
     "combinations": [{"patterns": {...}, "cases": [...]}],
     "title_specific_in_use": [{"variant", "cases", "frames"}]}"""

# rules/ambiguity.py  (A4)
def job_state(pieces):
    """Several pieces of evidence for one job, each {"state", "evidence_type", "build", "source"} -> {"state", "because"}"""

def case_verdict(comparison, measure_changes, jobs_before, jobs_after, guards):
    """-> {"answer": one word from your vocabulary, "because": [sentence, ...]}"""

def build_verdict(case_verdicts, impact):
    """-> {"answer", "because": [sentence, ...]}"""
```

Until a rule module exists the tool answers with the cautious stand-ins in
`defaults.py`: comparisons are "not judged", no floor moves, and the build's
answer is "not proven".

## 7. Comparison (what `compare` produces)

```json
{
  "build": "b8b6a4a",
  "rules_in_place": {"comparison": false, "guards": false, "transfer": false, "ambiguity": false},
  "cases": {
    "wwii-campaign-dlaa": {
      "case": "wwii-campaign-dlaa",
      "baseline": "<evidence_id>", "candidate": "<evidence_id>", "also_scored": [],
      "comparison": { },
      "measures": {"delivered_share_of_scene": {"baseline": 96.34, "candidate": 97.1, "change": 0.76, "unit": "percent",
                                                 "floor": {"kind": "min", "value": 96.0}, "against_floor": "holds"}},
      "jobs": {"jitter_applied": {"before": "verified", "after": "verified"}},
      "guards": [ ],
      "floor_action": { },
      "verdict": {"answer": "...", "because": [ ]}
    },
    "se5-dlaa": {"missing": "No scored evidence of build b8b6a4a for this case.", "baseline": "<evidence_id>"}
  },
  "impact": { },
  "verdict": {"answer": "...", "because": [ ]}
}
```

Facts are computed by the tool; judgments come from `rules/`:

- `comparison` is `rules.comparison.judge(baseline, candidate, case)`. Its
  `tolerance` is applied to the floor checks.
- `measures[...].against_floor` is `holds`, `below` or `unknown`. A measurement
  the candidate lacks is `unknown`, with the reason in `why`; unknown never holds.
- The baseline of a case is the evidence that set its floor; without a floor, the
  latest scored result of another build. The candidate is the build's latest saved
  result for the case; its other results are listed in `also_scored`.
- `guards` is `rules.guards.check(...)`, `floor_action` is
  `rules.comparison.floor_action(...)`, `verdict` is
  `rules.ambiguity.case_verdict(...)`, the build's `verdict` is
  `rules.ambiguity.build_verdict({case: verdict}, impact)`.

`compare` exits 0 when no case receded, 1 when one did, 2 when the build has no
scored evidence at all.

## 8. Dependency map and impact (`growth.dependencies/1`)

`growth/dependencies.json` lists areas: `{"id", "paths": [prefix or root-level
glob], "reaches": [provider, ...], "variants"?: [names], "capability"?, "why"}`.
The longest matching path classifies a changed file. `impact` answers per case:

| Answer | Meaning |
|---|---|
| `affected` | A changed file belongs to an area that reaches the case's provider. Eligibility counts, not which rule won in the last run. |
| `no_identified_dependency` | Every changed file is classified and none reaches the case. It rests on regression tests; it is not proof. |
| `impact_unknown` | A changed file matches no area. Never reported as "no impact". |

The list decides where play time is best spent. It does not renew any earlier
live verification: a result keeps the build it came from.
