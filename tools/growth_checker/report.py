"""The report page and the proposed ledger row.

One self-contained page: the build's answer, which games a change can reach,
every required job per route, each case beside its baseline and floor, the
pattern map and the ledger row. It states what is unknown as plainly as what
is verified. No script, no network call beyond the two typefaces.
"""
from html import escape as e

import scoring
from measure import value

GLYPH = {"verified": "✓", "failed": "✕", "unknown": "?", "untested": "–", "not_applicable": "·", None: ""}
WORD = {"verified": "verified", "failed": "failed", "unknown": "unknown", "untested": "untested", "not_applicable": "n/a"}
SHORT = {
    "home_menu_opens": "Home opens", "home_controls_take_effect": "Controls live", "status_truthful": "Status truthful",
    "performance_budget": "Fps budget", "presentation_modes": "Window modes", "no_crash_or_hang": "No crash",
    "mode_changes_render_size": "Mode resizes", "features_do_what_status_claims": "Features as claimed",
    "api_chosen": "API chosen", "camera_found": "Camera", "depth_found": "Depth", "motion_found": "Motion",
    "jitter_applied": "Jitter", "dlaa_delivered": "DLAA counted", "dlaa_reaches_picture": "DLAA on screen",
    "continuity": "Continuity", "cost": "Cost",
}
KEY_MEASURES = [
    ("scene_share_of_presented", "Judged scene, of presented"), ("delivered_share_of_scene", "Delivered, of scene"),
    ("delivered_share_of_presented", "Delivered, of presented"), ("longest_gap_seconds", "Longest gap (sampled)"),
    ("not_delivering_share_of_play", "Play time not delivering"), ("fps_play_median", "Fps, all play"),
    ("fps_median_while_delivering", "Fps while delivering"), ("fps_play_lowest", "Lowest fps in play"),
    ("time_to_first_delivery_seconds", "Time to first delivery"), ("play_seconds", "Play time measured"),
    ("frames_scene", "Scene frames"), ("frames_jittered", "Jittered frames"),
]
UNIT = {"percent": "%", "seconds": " s", "fps": "", "frames": "", "ms": " ms"}

CSS = """
/* Layout: a ledger sheet. The answer band first, then one job grid per route, then a sheet per case. */
:root {
  --paper: #f5f7fa; --sheet: #ffffff; --ink: #15202c; --muted: #566575; --line: #d3dae3; --accent: #1d5c8a;
  --good: #1c7449; --good-bg: #e3f3ea; --bad: #b0271f; --bad-bg: #fbe6e4; --open: #8a5d00; --open-bg: #fbf0d6;
  --idle: #5f6b79; --idle-bg: #e9edf2;
  --text: "IBM Plex Sans", "Segoe UI", system-ui, sans-serif; --data: "IBM Plex Mono", ui-monospace, Consolas, monospace;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --paper: #0f151c; --sheet: #17202a; --ink: #e5eaf0; --muted: #9babbb; --line: #2b3745; --accent: #74b6e2;
  --good: #5cc890; --good-bg: #163527; --bad: #ff9088; --bad-bg: #40201e; --open: #e6b84a; --open-bg: #3a2e12;
  --idle: #a3afbc; --idle-bg: #242f3b; color-scheme: dark } }
:root[data-theme="dark"] {
  --paper: #0f151c; --sheet: #17202a; --ink: #e5eaf0; --muted: #9babbb; --line: #2b3745; --accent: #74b6e2;
  --good: #5cc890; --good-bg: #163527; --bad: #ff9088; --bad-bg: #40201e; --open: #e6b84a; --open-bg: #3a2e12;
  --idle: #a3afbc; --idle-bg: #242f3b; color-scheme: dark }
body { background: var(--paper); color: var(--ink); font: 15px/1.5 var(--text); }
.page { max-width: 1080px; margin: 0 auto; padding-block: 28px 56px;
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px));
  display: flex; flex-direction: column; gap: 28px; }
h1, h2, h3 { margin: 0; text-wrap: balance; font-weight: 600; }
h1 { font-size: 26px; letter-spacing: -0.01em; }
h2 { font-size: 18px; }
h3 { font-size: 16px; }
p { margin: 0; max-width: 68ch; }
.eyebrow { font: 500 12px/1.3 var(--data); letter-spacing: 0.08em; text-transform: uppercase; color: var(--muted); }
.mono { font-family: var(--data); font-variant-numeric: tabular-nums; }
.muted { color: var(--muted); }
header { display: flex; flex-direction: column; gap: 6px; }
section { display: flex; flex-direction: column; gap: 12px; min-width: 0; }
.answer { background: var(--sheet); border: 1px solid var(--line); border-left: 6px solid var(--idle); padding: 16px 18px;
  display: flex; flex-direction: column; gap: 8px; }
.answer.receded { border-left-color: var(--bad); } .answer.grew, .answer.held { border-left-color: var(--good); }
.answer.open { border-left-color: var(--open); }
.answer strong { font-size: 22px; font-weight: 600; text-transform: capitalize; }
ul { margin: 0; padding-left: 20px; display: flex; flex-direction: column; gap: 4px; max-width: 76ch; }
.scroll { overflow-x: auto; border: 1px solid var(--line); background: var(--sheet); }
table { border-collapse: collapse; width: 100%; font-size: 13.5px; }
th, td { padding: 7px 10px; border-bottom: 1px solid var(--line); text-align: left; vertical-align: top; }
tr:last-child td { border-bottom: 0; }
th { font: 500 12px/1.3 var(--data); color: var(--muted); white-space: nowrap; }
td.num { font-family: var(--data); font-variant-numeric: tabular-nums; white-space: nowrap; }
.chip { display: inline-flex; align-items: center; gap: 5px; padding: 1px 8px; border-radius: 3px; white-space: nowrap;
  font: 500 12px/1.6 var(--data); background: var(--idle-bg); color: var(--idle); }
.chip.verified, .chip.holds { background: var(--good-bg); color: var(--good); }
.chip.failed, .chip.below { background: var(--bad-bg); color: var(--bad); }
.chip.unknown { background: var(--open-bg); color: var(--open); }
.case { background: var(--sheet); border: 1px solid var(--line); padding: 16px 18px; display: flex; flex-direction: column; gap: 14px; min-width: 0; }
.case > header { flex-direction: row; flex-wrap: wrap; align-items: baseline; gap: 6px 14px; }
.bar { display: flex; flex-direction: column; gap: 6px; }
.track { position: relative; height: 22px; background: var(--idle-bg); }
.track i { position: absolute; left: 0; top: 0; bottom: 0; display: block; }
.track .scene { background: color-mix(in srgb, var(--accent) 35%, var(--idle-bg)); }
.track .delivered { background: var(--accent); }
.legend { display: flex; flex-wrap: wrap; gap: 4px 18px; font: 13px/1.4 var(--data); color: var(--muted); }
.legend b { color: var(--ink); font-weight: 500; }
.key { display: inline-block; width: 10px; height: 10px; margin-right: 6px; vertical-align: baseline; }
details { border-top: 1px solid var(--line); padding-top: 10px; }
summary { cursor: pointer; font-weight: 500; }
summary:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
details > * + * { margin-top: 10px; }
pre { margin: 0; padding: 12px 14px; overflow-x: auto; background: var(--sheet); border: 1px solid var(--line); font: 12.5px/1.5 var(--data); }
.warn { border-left: 4px solid var(--open); padding: 6px 12px; background: var(--open-bg); color: var(--ink); }
.warn.blocks { border-left-color: var(--bad); background: var(--bad-bg); }
footer { color: var(--muted); font-size: 13px; display: flex; flex-direction: column; gap: 6px; }
"""


def chip(state, text=None):
    label = text or WORD.get(state, state or "")
    return f'<span class="chip {e(state or "")}">{GLYPH.get(state, "")} {e(label)}</span>'


def number(v, unit=None):
    if v is None:
        return '<span class="muted">unknown</span>'
    if isinstance(v, float):
        v = f"{v:,.1f}" if abs(v) < 1000 else f"{v:,.0f}"
    elif isinstance(v, int):
        v = f"{v:,}"
    return e(str(v)) + UNIT.get(unit or "", "")


def short(build):
    return (build or "no build line")[:8]


def _answer_class(answer):
    a = (answer or "").lower()
    return "receded" if "reced" in a else "grew" if "grew" in a or "grow" in a else "held" if "held" in a else "open"


def _job_grid(route, title, cases, comparison, results_by_id):
    jobs = scoring.required_jobs(route)
    head = "".join(f"<th>{e(SHORT[j])}</th>" for j in jobs)
    rows = []
    for case_id, case in sorted(cases.items()):
        if case["route"] != route:
            continue
        c = comparison["cases"].get(case_id, {})
        result = results_by_id.get(c.get("candidate"))
        if not result:
            rows.append(f'<tr><td><b>{e(case["game"])}</b></td><td colspan="{len(jobs)}" class="muted">'
                        f'No run of this build has been scored for this game.</td></tr>')
            continue
        cells = "".join(f'<td title="{e(result["jobs"][j]["note"])}">{chip(result["jobs"][j]["state"])}</td>'
                        if j in result["jobs"] else "<td></td>" for j in jobs)
        rows.append(f'<tr><td><b>{e(case["game"])}</b></td>{cells}</tr>')
    if not rows:
        return ""
    return (f'<section><h2>{e(title)}</h2><div class="scroll"><table><thead><tr><th>Game</th>{head}</tr></thead>'
            f'<tbody>{"".join(rows)}</tbody></table></div></section>')


def _coverage_bar(m):
    presented, scene, delivered = value(m, "frames_presented"), value(m, "frames_scene"), value(m, "frames_delivered")
    if not presented or delivered is None:
        return ""
    pct = lambda x: max(0.0, min(100.0, 100.0 * x / presented))
    scene_bar = f'<i class="scene" style="width:{pct(scene):.2f}%"></i>' if scene is not None else ""
    scene_key = (f'<span><span class="key" style="background:color-mix(in srgb, var(--accent) 35%, var(--idle-bg))"></span>'
                 f'judged scene <b>{scene:,}</b> ({pct(scene):.1f}%)</span>' if scene is not None else
                 '<span>judged scene: not logged by this provider</span>')
    return (f'<div class="bar"><div class="track" role="img" aria-label="Frames presented, judged scene and delivered, to scale">'
            f'{scene_bar}<i class="delivered" style="width:{pct(delivered):.2f}%"></i></div>'
            f'<div class="legend"><span><span class="key" style="background:var(--idle-bg);border:1px solid var(--line)"></span>'
            f'presented <b>{presented:,}</b></span>{scene_key}'
            f'<span><span class="key" style="background:var(--accent)"></span>DLSS delivered <b>{delivered:,}</b> ({pct(delivered):.1f}%)</span></div></div>')


def _case_sheet(case, c, result, baseline):
    man = result["manifest"]
    m = result["measurements"]
    rows = []
    for name, label in KEY_MEASURES:
        if name not in m:
            continue
        row = c["measures"].get(name, {})
        unit = m[name].get("unit")
        floor = row.get("floor")
        floor_cell = f'{e(floor["kind"])} {number(floor["value"], unit)}' if floor else '<span class="muted">none</span>'
        state = row.get("against_floor")
        state_cell = chip(state, {"holds": "holds", "below": "below floor", "unknown": "unknown"}[state]) if state else ""
        now = number(m[name]["value"], unit) if m[name]["state"] == "measured" else f'<span class="muted" title="{e(m[name].get("why", ""))}">unknown</span>'
        before = number(value(baseline["measurements"], name), unit) if baseline and baseline is not result else '<span class="muted">–</span>'
        rows.append(f'<tr><td>{e(label)}</td><td class="num">{before}</td><td class="num">{now}</td>'
                    f'<td class="num">{floor_cell}</td><td>{state_cell}</td></tr>')
    warns = "".join(f'<div class="warn {e(g.get("severity", ""))}">{e(g["message"])}</div>' for g in c.get("guards", []))
    notes = man["notes"] + (c["comparison"].get("reasons") or []) + (c["comparison"].get("limits") or [])
    note_list = "".join(f"<li>{e(n)}</li>" for n in notes)
    jobs = "".join(f'<tr><td>{e(scoring.JOB_TITLES[j])}</td><td>{chip(v["state"])}</td><td>{e(v["note"])}</td></tr>'
                   for j, v in result["jobs"].items())
    unknown = [f'{scoring.JOB_TITLES[j]}: {v["note"]}' for j, v in result["jobs"].items() if v["state"] == "unknown"]
    unknown += [f'{name.replace("_", " ")}: {mm.get("why", "")}' for name, mm in m.items() if mm["state"] == "unknown"]
    unknown += [f'{name.replace("_", " ")}: {pp.get("why", "")}' for name, pp in result["patterns"].items() if pp["state"] == "unknown"]
    unknown_list = "".join(f"<li>{e(u)}</li>" for u in unknown)
    deps = "".join(f'<li><span class="mono">{e(d["variant"])}</span>: {e(d["kind"].replace("_", " "))}, {d["frames"]:,} frames</li>'
                   for d in result["dependencies_in_use"]) or "<li>None recorded in this log.</li>"
    verdict = c["verdict"]
    same = baseline is result
    return f'''<article class="case" id="{e(case["id"])}">
<header><h3>{e(case["game"])}</h3><span class="eyebrow">Route {e(case["route"])} · {e(result["api"])} · {e(man["evidence_type"].replace("_", " "))}</span></header>
<p><span class="chip {_answer_class(verdict["answer"]) if _answer_class(verdict["answer"]) != "open" else "unknown"}">{e(verdict["answer"])}</span>
 {e(" ".join(verdict["because"]))}</p>
<p class="muted">Build <span class="mono">{e(short(man["producer"]["build"]))}</span>, read from {e(man["producer"]["build_source"].replace("_", " "))}.
 Session {e(str(man["session"]["id"]))}, {e(str(man["session"]["first_sample"]))} to {e(str(man["session"]["last_sample"]))}.
 {"This is the run that set the floor, so there is no earlier run beside it." if same else ""}</p>
{_coverage_bar(m)}
{warns}
<div class="scroll"><table><thead><tr><th>Measure</th><th>Baseline</th><th>This run</th><th>Floor</th><th></th></tr></thead><tbody>{"".join(rows)}</tbody></table></div>
<details><summary>Every job, with the reason</summary><div class="scroll"><table><tbody>{jobs}</tbody></table></div></details>
<details><summary>What this run cannot say ({len(unknown)})</summary><ul>{unknown_list}</ul></details>
<details><summary>Rules and title-specific code in use</summary><ul>{deps}</ul></details>
{f"<details><summary>Notes on this evidence</summary><ul>{note_list}</ul></details>" if notes else ""}
</article>'''


def _pattern_map(cases, comparison, results_by_id):
    names = ["api", "camera_admission", "camera_layout_from", "depth_convention", "jitter_route", "motion_source", "hud_composition", "presentation"]
    cols = [(cid, results_by_id.get(c.get("candidate"))) for cid, c in sorted(comparison["cases"].items())]
    cols = [(cid, r) for cid, r in cols if r]
    if not cols:
        return ""
    head = "".join(f"<th>{e(cases[cid]['game'])}</th>" for cid, _ in cols)
    body = []
    for name in names:
        cells = []
        for _, r in cols:
            p = r["patterns"].get(name, {"state": "unknown", "why": ""})
            if p["state"] == "observed":
                v = p["value"]
                text = ", ".join(f"{k} ({n:,})" for k, n in v.items()) if isinstance(v, dict) else str(v)
                cells.append(f"<td>{e(text)}</td>")
            else:
                cells.append(f'<td class="muted" title="{e(p.get("why", ""))}">{e(WORD.get(p["state"], p["state"]))}</td>')
        body.append(f'<tr><td>{e(name.replace("_", " "))}</td>{"".join(cells)}</tr>')
    return (f'<section><h2>Pattern map</h2><p class="muted">Which rendering patterns each game showed and which rule handled them. '
            f'Most rows stay unknown until the program logs them.</p><div class="scroll"><table><thead><tr><th>Pattern</th>{head}</tr></thead>'
            f'<tbody>{"".join(body)}</tbody></table></div></section>')


def ledger_row(comparison, cases, results_by_id, date):
    live, verified, floors, specific = [], [], [], []
    for cid, c in sorted(comparison["cases"].items()):
        r = results_by_id.get(c.get("candidate"))
        if not r:
            continue
        if r["manifest"]["evidence_type"] == "live_run":
            live.append(cases[cid]["game"])
        states = [j["state"] for j in r["jobs"].values()]
        verified.append(f'{cases[cid]["profile"]} {states.count("verified")} of {len(states)}')
        action = (c.get("floor_action") or {}).get("action")
        if action and action != "keep":
            floors.append(f'{cases[cid]["profile"]}: {action}')
        specific += [f'{d["variant"]} ({cases[cid]["profile"]})' for d in r["dependencies_in_use"]
                     if d["kind"] in ("title_specific", "shared_rule_with_adapter_hint", "provenance_unknown") and d["frames"] > 0]
    cells = ["(proposed)", f'`{short(comparison["build"])}`', date, ", ".join(live) or "none", "; ".join(verified) or "none",
             "; ".join(floors) or "none", "; ".join(specific) or "none recorded", "(written at packaging)"]
    return "| " + " | ".join(cells) + " |"


def page(comparison, cases, results, date, title="Growth report"):
    """-> the page as HTML (content only: the publishing skeleton or a browser supplies the rest)."""
    by_id = {r["manifest"]["evidence_id"]: r for r in results}
    verdict = comparison["verdict"]
    missing_rules = [name for name, there in comparison["rules_in_place"].items() if not there]
    parts = [f'''<title>{e(title)}</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500;600&display=swap">
<style>{CSS}</style>
<div class="page">
<header><span class="eyebrow">NeuralAlias growth checker · {e(date)}</span>
<h1>Build <span class="mono">{e(short(comparison["build"]))}</span> against its baseline</h1></header>
<div class="answer {_answer_class(verdict["answer"])}"><span class="eyebrow">The answer for this build</span>
<strong>{e(verdict["answer"])}</strong><ul>{"".join(f"<li>{e(b)}</li>" for b in verdict["because"])}</ul>''']
    if missing_rules:
        parts.append(f'<p class="muted">Judgment rules not in this checker yet: {e(", ".join(missing_rules))}. Until they are, '
                     f'the only answers it gives are "receded", on a broken floor or a verified job that now fails, and "not proven".</p>')
    parts.append("</div>")
    imp = comparison.get("impact")
    if imp:
        groups = {"affected": "Play these: the change can reach them", "impact_unknown": "Impact unknown: treat as possibly affected",
                  "no_identified_dependency": "No identified dependency (rests on regression tests, not proof)"}
        items = []
        for key, label in groups.items():
            names = [cases[cid]["game"] for cid, v in sorted(imp["cases"].items()) if v["answer"] == key]
            if names:
                items.append(f"<li><b>{e(label)}:</b> {e(', '.join(names))}</li>")
        parts.append(f'<section><h2>Which games this change can reach</h2><ul>{"".join(items)}</ul></section>')
    parts.append(_job_grid("B", "Route B: DLSS added by the program", cases, comparison, by_id))
    parts.append(_job_grid("A", "Route A: the game's own DLSS", cases, comparison, by_id))
    sheets = []
    for cid, c in sorted(comparison["cases"].items()):
        r = by_id.get(c.get("candidate"))
        if r:
            sheets.append(_case_sheet(cases[cid], c, r, by_id.get(c.get("baseline"))))
    if sheets:
        parts.append(f'<section><h2>Case by case</h2>{"".join(sheets)}</section>')
    parts.append(_pattern_map(cases, comparison, by_id))
    row = ledger_row(comparison, cases, by_id, date)
    parts.append('<section><h2>Proposed ledger row</h2><p class="muted">For docs/BUILD-LEDGER.md. It becomes a baseline only when the retest supports it.</p>'
                 '<pre>| Baseline | Build hash | Date | Cases run by Cort | Jobs verified | Floors proposed or confirmed | Title-specific code in use | What changed for the player |\n'
                 f'{e(row)}</pre></section>')
    parts.append('<footer><p>Verified means the evidence shows it. Unknown means the evidence cannot say, and unknown never counts as a pass. '
                 'A sampled number is estimated from the periodic status samples and is only as fine as the sample period.</p>'
                 '<p>Numbers come from saved logs, named by hash in each result file under growth/results. No log is stored in the repository.</p></footer></div>')
    return "\n".join(p for p in parts if p)
