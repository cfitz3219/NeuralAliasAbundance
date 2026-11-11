"""The GameCheck report page and the proposed ledger row.

The first screen is a visual overview (Cort's design brief, 2026-11-11): the
GameCheck name, the build being compared and the overall result on top, then
one compact card per game, games that regressed or need a retest first. Each
card gives a plain-language verdict and the two measurements that explain it,
whole-play fps and DLAA frame coverage. Status colours are fixed (green
improved, blue held, amber needs evidence, red regression or failure) and are
always paired with a word and an icon.

Everything underneath is unchanged in wording and kept in expandable details:
which games a change can reach, every required job per route, each case beside
its baseline and floor, the pattern map and the ledger row. It states what is
unknown as plainly as what is verified. No script, no network call beyond the
two typefaces.
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
  --paper: #f3f5f9; --sheet: #ffffff; --ink: #142030; --muted: #586679; --line: #d6dde7; --accent: #2457c5;
  --good: #16794a; --good-bg: #dff3e8; --held: #1f5fbf; --held-bg: #e1ebfb; --open: #8a5a00; --open-bg: #fcefd2;
  --bad: #b42318; --bad-bg: #fde4e1; --idle: #5f6b79; --idle-bg: #e8ecf2; --hero: #0f1b2d; --hero-ink: #f4f7fb;
  --hero-muted: #a9b6c8;
  --text: "IBM Plex Sans", "Segoe UI", system-ui, sans-serif; --data: "IBM Plex Mono", ui-monospace, Consolas, monospace;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --paper: #0d131b; --sheet: #161f2a; --ink: #e6ebf1; --muted: #9cabbc; --line: #2a3646; --accent: #7ea6f2;
  --good: #5fd09a; --good-bg: #133628; --held: #7fb0ff; --held-bg: #172a47; --open: #f0bf55; --open-bg: #3a2c10;
  --bad: #ff8f86; --bad-bg: #44201d; --idle: #a3afbc; --idle-bg: #232e3b; --hero: #111c2b; --hero-ink: #f4f7fb;
  --hero-muted: #9fb0c6; color-scheme: dark } }
:root[data-theme="dark"] {
  --paper: #0d131b; --sheet: #161f2a; --ink: #e6ebf1; --muted: #9cabbc; --line: #2a3646; --accent: #7ea6f2;
  --good: #5fd09a; --good-bg: #133628; --held: #7fb0ff; --held-bg: #172a47; --open: #f0bf55; --open-bg: #3a2c10;
  --bad: #ff8f86; --bad-bg: #44201d; --idle: #a3afbc; --idle-bg: #232e3b; --hero: #111c2b; --hero-ink: #f4f7fb;
  --hero-muted: #9fb0c6; color-scheme: dark }
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
.chip.failed, .chip.below, .chip.receded { background: var(--bad-bg); color: var(--bad); }
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

/* Overview: a dark title band with the overall result, a tally, then one card per game. */
.hero { background: var(--hero); color: var(--hero-ink); border-radius: 14px; padding: 22px 24px;
  display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 18px 28px; align-items: center; }
.brand { display: flex; align-items: center; gap: 10px; font-size: 26px; font-weight: 600; letter-spacing: -0.01em; }
.brand svg { width: 30px; height: 30px; flex: none; }
.hero .eyebrow { color: var(--hero-muted); }
.hero p { color: var(--hero-muted); }
.hero .mono { color: var(--hero-ink); }
.verdict { display: flex; flex-direction: column; gap: 8px; align-items: flex-start; min-width: 0; }
.big { display: inline-flex; align-items: center; gap: 10px; padding: 10px 18px; border-radius: 999px;
  font-size: 22px; font-weight: 600; background: var(--idle-bg); color: var(--idle); }
.big svg { width: 24px; height: 24px; flex: none; }
.tally { display: flex; flex-wrap: wrap; gap: 10px; }
.tally span { display: inline-flex; align-items: center; gap: 8px; padding: 8px 14px; border-radius: 10px;
  background: var(--sheet); border: 1px solid var(--line); font-size: 14px; }
.tally b { font-size: 20px; font-variant-numeric: tabular-nums; }
.tally svg { width: 18px; height: 18px; }
.cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(100%, 320px), 1fr)); gap: 16px; }
.card { background: var(--sheet); border: 1px solid var(--line); border-top: 5px solid var(--idle); border-radius: 12px;
  padding: 16px 18px; display: flex; flex-direction: column; gap: 12px; min-width: 0; }
.card h3 { font-size: 18px; }
.card .top { display: flex; flex-direction: column; align-items: flex-start; gap: 8px; }
.card p { font-size: 14.5px; }
.pill { display: inline-flex; align-items: center; gap: 6px; padding: 3px 11px 3px 8px; border-radius: 999px;
  font-size: 13px; font-weight: 600; white-space: nowrap; background: var(--idle-bg); color: var(--idle); }
.pill svg { width: 15px; height: 15px; flex: none; }
.s-good { --tone: var(--good); --tone-bg: var(--good-bg); }
.s-held { --tone: var(--held); --tone-bg: var(--held-bg); }
.s-open { --tone: var(--open); --tone-bg: var(--open-bg); }
.s-bad  { --tone: var(--bad);  --tone-bg: var(--bad-bg); }
.card.s-good, .card.s-held, .card.s-open, .card.s-bad { border-top-color: var(--tone); }
.pill.s-good, .pill.s-held, .pill.s-open, .pill.s-bad,
.big.s-good, .big.s-held, .big.s-open, .big.s-bad { background: var(--tone-bg); color: var(--tone); }
.tally .s-good svg, .tally .s-held svg, .tally .s-open svg, .tally .s-bad svg { color: var(--tone); }
.metrics { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
.metric { background: var(--paper); border-radius: 10px; padding: 10px 12px; display: flex; flex-direction: column; gap: 4px; min-width: 0; }
.metric .label { font-size: 12px; color: var(--muted); letter-spacing: 0.02em; }
.metric .value { font-size: 26px; font-weight: 600; font-variant-numeric: tabular-nums; line-height: 1.1; }
.metric .value small { font-size: 14px; font-weight: 500; color: var(--muted); }
.metric .sub { font-size: 12px; color: var(--muted); }
.metric.stale .value { color: var(--muted); }
.metric.low .value { color: var(--bad); }
.meter { height: 6px; border-radius: 3px; background: var(--idle-bg); position: relative; overflow: hidden; }
.meter i { position: absolute; inset: 0 auto 0 0; display: block; border-radius: 3px; background: var(--accent); }
.meter i.ok { background: var(--good); } .meter i.low { background: var(--bad); } .meter i.stale { background: var(--idle); }
.meter b { position: absolute; top: -2px; bottom: -2px; width: 2px; background: var(--ink); opacity: 0.55; }
.card .foot { display: flex; justify-content: space-between; gap: 8px; flex-wrap: wrap; font-size: 12.5px; color: var(--muted); }
.card a { color: var(--accent); }
.evidence > details { background: var(--sheet); border: 1px solid var(--line); border-radius: 12px; padding: 12px 16px; }
.evidence > details > summary { font-size: 16px; }
details.case { border-top: 0; border-radius: 12px; }
details.case > summary { display: flex; flex-wrap: wrap; align-items: center; gap: 8px 12px; list-style-position: outside; }
@media (max-width: 640px) { .hero { grid-template-columns: 1fr; } .metric .value { font-size: 22px; } }
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
    return "receded" if "reced" in a or "fail" in a else "grew" if "grew" in a or "grow" in a else "held" if "held" in a else "open"


# Status palette: fixed meaning, always a word and an icon beside the colour.
ICON = {
    "good": '<path d="M12 19V5M5 12l7-7 7 7"/>',
    "held": '<path d="M5 9h14M5 15h14"/>',
    "open": '<path d="M9.2 9a3 3 0 1 1 4.3 2.7c-.9.4-1.5 1.2-1.5 2.2V15"/><path d="M12 18.5v.5"/>',
    "bad": '<path d="M6 6l12 12M18 6L6 18"/>',
}
STATUS = {
    "grew": ("good", "Improved"), "held": ("held", "Held"), "not proven": ("open", "Needs evidence"),
    "missing": ("open", "Needs a retest"), "receded": ("bad", "Regressed"), "failing": ("bad", "Failing"),
}
ORDER = {"receded": 0, "failing": 1, "not proven": 2, "missing": 3, "held": 4, "grew": 5}


def _status_key(answer):
    return answer if answer in STATUS else "not proven"


def icon(tone):
    return (f'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" '
            f'stroke-linejoin="round" aria-hidden="true">{ICON[tone]}</svg>')


def _pill(tone, word, cls="pill"):
    return f'<span class="{cls} s-{tone}">{icon(tone)}{e(word)}</span>'


def result_file(result):
    """Where a scored result is kept, relative to the repository."""
    m = result["manifest"]
    return f'growth/results/{short(m["producer"]["build"])}/{m["case"] or "unassigned"}/{m["evidence_id"]}@{m.get("scorer") or "?"}.json'


def _plain(answer, verdict, result):
    """One plain sentence for a card. The rule's own wording stays in the details below."""
    because = verdict.get("because") or []
    if answer == "failing":
        continuity = result["jobs"].get("continuity", {})
        if continuity.get("state") == "failed":
            return continuity["note"]
        failed = [scoring.JOB_TITLES.get(j, j) for j, v in result["jobs"].items() if v["state"] == "failed"]
        return "Required checks failed: " + "; ".join(failed) + "." if failed else " ".join(because)
    if answer == "receded":
        return "Fell back: " + " ".join(because[:2])
    return " ".join(because[:2])


def _metric(label, v, unit_word, sub, fill, mark=None, tone=None, stale=False):
    shown = '<span class="muted">–</span>' if v is None else f'{v:,.0f}' if unit_word == "fps" else f'{v:.1f}<small>%</small>'
    bar = ""
    if v is not None and fill is not None:
        marker = f'<b style="left:{mark:.1f}%"></b>' if mark is not None else ""
        bar = f'<div class="meter"><i class="{"stale" if stale else tone or ""}" style="width:{max(0.0, min(100.0, fill)):.1f}%"></i>{marker}</div>'
    return (f'<div class="metric{" stale" if stale else ""}{" low" if tone == "low" and not stale else ""}"><span class="label">{e(label)}</span>'
            f'<span class="value">{shown}{"" if v is None or unit_word != "fps" else " <small>fps</small>"}</span>{bar}'
            f'<span class="sub">{sub}</span></div>')


def _card_metrics(case, result, stale):
    m = result["measurements"] if result else {}
    fps = value(m, "fps_play_median")
    budget = (case.get("budget") or {}).get("fps_min")
    cov = value(m, "delivered_share_of_presented")
    cov_label = "of all presents · includes menus and loading"
    scale = max(3.0 * (budget or 40), fps or 0)
    small = lambda tone: icon(tone).replace('<svg', '<svg style="width:12px;height:12px;vertical-align:-1px"')
    judged = ((result or {}).get("jobs") or {}).get("performance_budget", {}).get("state")
    if fps is None:
        fps_sub = "not measured" if result else "no run yet"
    elif not budget:
        fps_sub = "no budget set"
    elif judged == "verified":
        fps_sub = f"{small('good')} meets the {budget} fps budget"
    elif judged == "failed":
        fps_sub = f"{small('bad')} below the {budget} fps budget"
    else:
        fps_sub = (f"budget not judged: DLAA did not run" if not cov else f"budget of {budget} fps not judged")
    tone = "ok" if judged == "verified" else "low" if judged == "failed" else None
    continuity = ((result or {}).get("jobs") or {}).get("continuity", {})
    reported = continuity.get("source") == "owner_verdict"
    state = continuity.get("state")
    if reported and state == "failed":
        label, detail, colour = "Flickering reported", continuity.get("note", ""), "bad"
    elif reported and state == "verified":
        label, detail, colour = "Stable in this run", continuity.get("note", ""), "good"
    else:
        label, detail, colour = "Stability unverified", "Delivery totals cannot establish visible continuity.", "open"
    scene_share = value(m, "delivered_share_of_scene")
    missed_time = value(m, "not_delivering_share_of_play")
    extra = []
    if scene_share is not None:
        extra.append(f"{scene_share:.1f}% of recognised scene opportunities received output.")
    if missed_time is not None:
        extra.append(f"{missed_time:.1f}% of observed time was in intervals with under 98% delivery.")
    stability = (f'<div class="continuity"><p>{_pill(colour, label)}</p><p>{e(detail)}</p>'
                 f'<p class="muted">{e(" ".join(extra))}</p></div>')
    return ('<div class="metrics">'
            + _metric("Whole-play fps", fps, "fps", fps_sub, None if fps is None else 100.0 * fps / scale,
                      100.0 * budget / scale if budget else None, tone, stale)
            + _metric("Counted DLAA deliveries", cov, "%", cov_label if cov is not None else ("not measured" if result else "no run yet"),
                      None, None, None, stale)
            + '</div>' + (stability if result and case["route"] == "B" else ""))


def _overview(comparison, cases, results, by_id):
    cards = []
    tally = {"bad": 0, "open": 0, "held": 0, "good": 0}
    for cid, c in comparison["cases"].items():
        case = cases[cid]
        if "missing" in c:
            key = "missing"
            earlier = sorted((r for r in results if r["manifest"]["case"] == cid and r["status"] == "scored"),
                             key=lambda r: r["manifest"]["session"]["saved_at"] or "")
            result = earlier[-1] if earlier else None
            if result:
                said = (f'Not run on {short(comparison["build"])} yet. The numbers are from the last scored run, build '
                        f'{short(result["manifest"]["producer"]["build"])}.')
            else:
                said = f'Not run on {short(comparison["build"])} yet, and no earlier run of this game is scored.'
            link = ""
        else:
            key = _status_key(c["verdict"]["answer"])
            result = by_id.get(c.get("candidate"))
            said = _plain(key, c["verdict"], result)
            link = f'<a href="#{e(cid)}">Full evidence</a>'
        tone, word = STATUS[key]
        tally[tone] += 1
        stale = key == "missing"
        build_note = (f'Last run: build <span class="mono">{e(short(result["manifest"]["producer"]["build"]))}</span>'
                      if result else "No scored run")
        cards.append((ORDER[key], case["game"], f'''<article class="card s-{tone}">
<div class="top">{_pill(tone, word)}<div><h3>{e(case["game"])}</h3><span class="eyebrow">Route {e(case["route"])} · {e(case.get("api", ""))} · {e((case.get("settings") or {}).get("mode", ""))}</span></div></div>
<p>{e(said)}</p>
{_card_metrics(case, result, stale)}
<div class="foot"><span>{build_note}</span>{link}</div>
</article>'''))
    cards.sort(key=lambda x: (x[0], x[1]))
    names = {"bad": "Regressed or failing", "open": "Needs evidence or a retest", "held": "Held", "good": "Improved"}
    tally_html = "".join(f'<span class="s-{k}">{icon(k)}<b>{n}</b> {e(names[k])}</span>' for k, n in tally.items())
    return f'''<section aria-label="Overview"><div class="tally">{tally_html}</div></section>
<section><h2>Games, most urgent first</h2><div class="cards">{"".join(card for _, _, card in cards)}</div></section>'''


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
    seen = []
    for g in c.get("guards", []):
        if (g.get("severity"), g["message"]) not in seen:
            seen.append((g.get("severity"), g["message"]))
    warns = "".join(f'<div class="warn {e(sev or "")}">{e(msg)}</div>' for sev, msg in seen)
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
    files = [f'This run: <span class="mono">{e(result_file(result))}</span>']
    if baseline and not same:
        files.append(f'Baseline: <span class="mono">{e(result_file(baseline))}</span>')
    files_list = "".join(f"<li>{x}</li>" for x in files)
    tone, word = STATUS[_status_key(verdict["answer"])]
    return f'''<details class="case" id="{e(case["id"])}">
<summary>{_pill(tone, word)} <b>{e(case["game"])}</b> <span class="eyebrow">Route {e(case["route"])} · {e(result["api"])} · {e(man["evidence_type"].replace("_", " "))} · build <span class="mono" style="text-transform:none">{e(short(man["producer"]["build"]))}</span></span></summary>
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
<details><summary>Evidence files</summary><ul>{files_list}</ul></details>
</details>'''


def _pattern_map(cases, comparison, results_by_id):
    names = ["api", "scene_placement", "camera_admission", "camera_layout_from", "depth_convention", "jitter_route", "motion_source", "hud_composition", "presentation"]
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


def page(comparison, cases, results, date, title="GameCheck", controls="", external_fonts=True):
    """-> the page as HTML (content only: the publishing skeleton or a browser supplies the rest)."""
    by_id = {r["manifest"]["evidence_id"]: r for r in results}
    verdict = comparison["verdict"]
    missing_rules = [name for name, there in comparison["rules_in_place"].items() if not there]
    tone, word = STATUS[_status_key(verdict["answer"])]
    ran = sum(1 for c in comparison["cases"].values() if "missing" not in c)
    waiting = len(comparison["cases"]) - ran
    fonts = ('<link rel="stylesheet" href="https://fonts.googleapis.com/css2?'
             'family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500;600&display=swap">'
             if external_fonts else "")
    parts = [f'''<title>{e(title)} {e(short(comparison["build"]))}</title>
<meta name="claude:viewport-fit-sides" content="cover">
{fonts}
<style>{CSS}</style>
<div class="page">
<header class="hero">
<div class="verdict">
<span class="eyebrow">NeuralAlias · growth report · {e(date)}</span>
<div class="brand"><svg viewBox="0 0 32 32" aria-hidden="true"><rect width="32" height="32" rx="8" fill="#2f6fe4"/><path d="M9 16.5l4.5 4.5L23 11.5" fill="none" stroke="#fff" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round"/></svg>GameCheck</div>
<p>Build <span class="mono">{e(short(comparison["build"]))}</span> compared with each game's floor and last accepted run.
 {ran} of {len(comparison["cases"])} games have a scored run of this build{f"; {waiting} are waiting for one" if waiting else ""}.</p>
</div>
<div class="verdict"><span class="eyebrow">Overall result</span>{_pill(tone, word, "big")}</div>
</header>''']
    if controls:
        parts.append(controls)
    parts.append(_overview(comparison, cases, results, by_id))
    parts.append('<section class="evidence"><h2>Full evidence</h2>')
    answer_box = [f'''<details><summary>The answer for this build, in the rules' own words</summary>
<div class="answer {_answer_class(verdict["answer"])}"><span class="eyebrow">The answer for this build</span>
<strong>{e(verdict["answer"])}</strong><ul>{"".join(f"<li>{e(b)}</li>" for b in verdict["because"])}</ul>''']
    if missing_rules:
        answer_box.append(f'<p class="muted">Judgment rules not in this checker yet: {e(", ".join(missing_rules))}. Until they are, '
                          f'the only answers it gives are "receded" (a broken floor, or a verified job that now fails), "failing" (a required job '
                          f'failed and there is nothing earlier to compare) and "not proven".</p>')
    answer_box.append("</div></details>")
    parts.append("".join(answer_box))
    imp = comparison.get("impact")
    if imp:
        groups = {"affected": "Play these: the change can reach them", "impact_unknown": "Impact unknown: treat as possibly affected",
                  "no_identified_dependency": "No identified dependency (rests on regression tests, not proof)"}
        items = []
        for key, label in groups.items():
            names = [cases[cid]["game"] for cid, v in sorted(imp["cases"].items()) if v["answer"] == key]
            if names:
                items.append(f"<li><b>{e(label)}:</b> {e(', '.join(names))}</li>")
        parts.append(f'<details open><summary>Which games this change can reach</summary><ul>{"".join(items)}</ul></details>')
    grids = _job_grid("B", "Route B: DLSS added by the program", cases, comparison, by_id) + \
        _job_grid("A", "Route A: the game's own DLSS", cases, comparison, by_id)
    if grids:
        parts.append(f'<details><summary>Job checks by route</summary>{grids}</details>')
    sheets = []
    for cid, c in sorted(comparison["cases"].items()):
        r = by_id.get(c.get("candidate"))
        if r:
            sheets.append(_case_sheet(cases[cid], c, r, by_id.get(c.get("baseline"))))
    if sheets:
        parts.append("".join(sheets))
    pm = _pattern_map(cases, comparison, by_id)
    if pm:
        parts.append(f'<details><summary>Pattern map</summary>{pm}</details>')
    row = ledger_row(comparison, cases, by_id, date)
    parts.append('<details><summary>Proposed ledger row</summary><section><p class="muted">For docs/BUILD-LEDGER.md. It becomes a baseline only when the retest supports it.</p>'
                 '<pre>| Baseline | Build hash | Date | Cases run by Cort | Jobs verified | Floors proposed or confirmed | Title-specific code in use | What changed for the player |\n'
                 f'{e(row)}</pre></section></details></section>')
    parts.append('<footer><p>Verified means the evidence shows it. Unknown means the evidence cannot say, and unknown never counts as a pass. '
                 'A sampled number is estimated from the periodic status samples and is only as fine as the sample period.</p>'
                 '<p>Numbers come from saved logs, named by hash in each result file under growth/results. No log is stored in the repository.</p></footer></div>')
    return "\n".join(p for p in parts if p)
