# NeuralAliasAbundance: master plan

Short version (2026-11-18). This page keeps what is still in force: the goal, the two routes, the method and growth checker (section 0), how we work, the environment and the final builds per game. **For who is doing what right now, read the "Current state" section at the top of COMMS.md.**

The full previous text (905 lines, 139 KB: phase plans 1-4, task lists, gate history, dated milestone notes for material enhancement, camera-independent DLAA, Sniper, PBZ, Ghost, NVRHI and more) is kept word for word in [`docs/comms-archive/MASTER-PLAN-through-2026-11-18.md`](docs/comms-archive/MASTER-PLAN-through-2026-11-18.md). Its line numbers are unchanged, so older links like "MASTER-PLAN §3" or "section 2, task C9" can be followed there.

**Keeping it short:** add a dated milestone here only when it changes the plan (a new route, a new rule, a new final build). Day-to-day progress goes in COMMS.md and your WORKLOG.


> **Goal:** broad DLSS compatibility through native integration where the game provides it and shared input discovery and reconstruction where it does not. Keep camera, depth, motion, jitter and scene delivery reusable across the graphics APIs that Route B supports.
> **Course change (2026-10-25, Cort):** discovery uses **established, field-proven algorithms first**: ReShade `generic_depth` depth scoring, UEVR/Geo-11 combined view-projection and inverse-pair camera detection, and Special K/Luma pre-HUD boundary tracking (reference: `docs/ARGON-DROPIN-HEURISTICS.md`). Our own verification checks stay as secondary corroboration and logging, never as all-or-nothing gates that report zero in real games.
> Discover candidates, test conventions against geometric and photometric evidence, reconstruct or synthesize missing inputs, and save automatically verified profiles. Missing inputs create engineering tasks. Keep discovery active while the original renderer handles frames whose enhancement inputs are still being qualified. Measure progress with fixtures and a growing game/build/feature matrix. Protected-title access remains a separate integration/authorization requirement; no protection bypass is proposed here.

**Latest execution authority — 2026-11-09:** Cort now explicitly directs: "Leave in game testing to me." For the current WWII performance/integration work, Cort owns game launches, control and live testing. Agents continue source changes, automated CPU/DX12 regressions, coordination and staging; do not perform diagnostic game deployments or live comparisons. This supersedes the earlier standing live-test permission below, retained as history. Target remains at least 40 FPS / 25 ms whole frame with continuous DLAA; no measured acceptance yet.

**Current execution authority — 2026-11-07:** Cort explicitly authorizes Astra to take over after Fable implements the reviewed Sniper stability fixes, including reversible deployment, launching/controlling/restarting games, in-game testing, captures, automated regressions and further repairs without asking Cort to perform the tests. This supersedes the older "Cort retains game testing" restrictions for this repair and integration work. Fable owns the current implementation until his completed handoff; Astra then owns integration, testing, remaining corrections and the combined package. Start with Sniper's continuous DLAA/Quality delivery and preserve its native HUD, then address remaining observed stability/image issues and relevant shared-route regressions. Keep scope on completing the existing product, reuse established machinery, and preserve rollback/settings/saves. Handoff details and the active evidence-backed corrections are in the latest COMMS entries.

**Required guide for ALL future project work (2026-11-07, Cort's instruction):** Every contributor/agent must read and apply the five rules at the top of [COLLABORATION.md](COLLABORATION.md) when starting or resuming any NeuralAliasAbundance task: reuse existing mechanisms; trace the complete cause-and-effect chain; make each diagnostic run decide the next action; change the approach when evidence stops improving; deliver reusable, visibly verified work without scope expansion. This covers planning, research, architecture, implementation, performance, UI, installers, testing and handoffs across games and phases. Carry this requirement into future handoffs. Inspired by Fable 5.1's discovery; no extra approval or review cycle.

## Current DLAA test sequence (2026-11-21, Cort)

Finish the current integration, remaining Referee fixes and automated checks before staging the candidate for Cort's live testing. Use the same integrated source for the first attempt on each game.

1. Total War: Pharaoh Dynasties
2. Total War: Three Kingdoms
3. Assassin's Creed IV: Black Flag

Use the existing Results logs and what Cort sees during play. Check that native-resolution DLAA reaches the presented image, stays on during play and recovers after returning from menus, with responsive pacing and no crashes or hangs. Record the build and the first result for each game. If a title exposes a new rendering pattern, work from that evidence and check the shared change against the titles already tested.

Route B DLSS upscaling stays disabled during this work. Once all three are stable, discuss render-size reduction using the prior experiments and the three games' evidence. The design problem is how to lower actual scene rendering across games while preserving a full-resolution presentation and HUD, then verify that the DLSS output reaches the screen.

## Current architecture — two routes (2026-11-10, Cort)

NeuralAlias has two supported routes, defined by whether the game ships native DLSS.

| Route | What NeuralAlias does | Compatibility examples |
|---|---|---|
| **A — Native DLSS** | Use the game's existing DLSS integration and its inputs. Apply supported controls through the DLSS integration or a qualified engine adapter. Verify live quality changes against the dimensions the game actually renders. | Ghost of Tsushima, Phantom Blade Zero |
| **B — Add DLSS** | Acquire scene inputs from a compatible game upscaler or through discovery, reconstruct missing data, and deliver DLAA or DLSS upscaling. DX11 and DX12 are input providers within this route. Both feed the shared DLSS backend and follow the same scene delivery requirements. | Anno 1800, Sniper Elite 5, Sniper Elite Resistance, Call of Duty WWII |

Use the same source baseline for both routes in the compatibility audit. Record the graphics API, provider, build and verified features for each game. A DX11 capture or translation experiment belongs under Route B, with its own evidence and status. The native DX11 provider in WWII is the tested starting point for that API. The short WWII run still had a camera-related dropout, so keep continuity open on the scorecard.

Preserve proven engine and game adapters while assessing what transfers. The examples identify which route owns each game. Individual features earn compatibility credit from gameplay evidence on the candidate being scored.

**Referee parity (2026-11-19, Cort):** Route B runs the same decision policy for DX11 and DX12. Both can nominate a verified placement for localized flicker or incomplete delivery, compare the displayed result, keep an improvement, and move on to other available corrections. API adapters supply actual resource, reader, submission and presentation evidence and execute the selected action. New recovery behavior belongs in the shared Referee and should receive equivalent-evidence tests for both paths.

**Unified main branch (2026-11-20, Cort):** After Fable's final handoff, integrate the remaining work and the shared Referee into GitHub's `main`. Maintain one source baseline and build path for both routes and graphics APIs. Preserve the tested adapters and publish integration work to GitHub as it progresses. Draft PR 181 is the current integration candidate.

## 0. The method: how the program grows, and how we prove it (2026-11-11, v3)

**Cort, 2026-11-11:** "We need a planned method after we confirm it works in all games tested. After that we need an actual way to solidly confirm we are growing and not receding. Something professional... a method to the madness that's consistent. That a program becomes intelligent and adaptive to any game type it comes in contact with. We do not want a simple hodge podge of code scripts lazily bound together." Later: the checker "would be separate from the main program."

Status: v3 joins Claude's draft, Sol's corrections, Claude's feasibility review and Astra's review (all in COMMS, 2026-11-11), and the build plan Cort asked for at 02:49 PT: most of the work is Claude's, the hard judgment rules are Astra's, Claude compiles the result (0.10). It adds no review gate: it replaces "does it feel better" with evidence each build must show.

### 0.1 Two parts, kept apart

| Part | What it is | Where it runs |
|---|---|---|
| **NeuralAlias** | The program in the game. It owns discovery, rule choice, validation and what it remembers per game. Its logs say what it found, which rule found it, and why the other rules did not. | Inside the game |
| **Growth checker** | A development tool and a separate deliverable. It reads saved evidence (run logs, test and replay results) and writes the comparison between builds, a report and a ledger row. It adds no code, hook, service or installer dependency to NeuralAlias, and NeuralAlias runs without it. Its report needs no model or network call. | Our workspaces, Sol's machine, CI for its own tests. It scores saved evidence with the game closed. |

A better rule inside NeuralAlias is a NeuralAlias change with its own evidence. The checker only measures what changed between builds. It does not make the program adaptive; the rules do. It keeps them honest and points the work at the rule instead of the game.

### 0.2 What "adaptive" means inside NeuralAlias

For each job the program holds a short ladder of general rules, strongest proof first. In a running game it tries them on what the game shows, admits only what is proven, logs which rule did the work and why the others did not, and remembers the winner for that game together with the evidence to recheck it. A game or build identifier may find a remembered entry; the render structure it sees decides whether the entry still applies.

Three laws keep it one program:

1. **A rule is chosen by what the program observes in the running game, never by the game's name or exe.** Every such choice has a name, a `VARIANT:` comment and one row in `docs/VARIANTS.md`; CI checks that the marked names and the register agree (it checks presence, not meaning).
2. **A new game that fails is fixed by improving a rule or adding one to a ladder.** An adapter may supply a hint; the shared rule still verifies it. A small necessary adapter is acceptable; removing one at the cost of a verified feature is not progress. Title-specific code is counted where it is *used*, including the Route A adapters and anything sitting in shared files.
3. **Every job says how it was done and why not otherwise.** A missing reason is a telemetry gap: reported as such, and the rest of the run is still scored.

### 0.3 Required jobs per route

Each job on each run is **verified, failed, untested, not applicable or unknown**. A game is never marked down for a job its route does not need. Missing evidence is "unknown"; it never counts as a pass.

| Route | Required jobs |
|---|---|
| **Both** | Home menu opens and its controls take effect live; the status says truthfully what runs; the case's performance budget is met (Cort's minimum of 40 to 50 fps unless he sets another budget for that case); each presentation mode the evidence can tell apart; no crash or hang |
| **A: game ships DLSS** | A requested mode changes the game's render size; each enabled feature does what its status claims |
| **B: DLSS added** | API chosen automatically; camera, depth, motion and jitter found; DLAA reaches the presented picture; continuity; cost |

### 0.4 The steps

| # | Step | Done when |
|---|---|---|
| 1 | **Baseline 1.** The build that passes Cort's retest (candidate `b8b6a4a`) is recorded with its per-case results. Floors are set only for what the logs measure. | One result file per case, floors proposed, one ledger row |
| 2 | **Automated part of every change.** CPU tests, the variants check, and the changed rule replayed on recorded inputs for the new case and every existing case it can affect. Where useful a rule repair adds one generalization case: the same recorded content at other slots or offsets, or among decoy buffers, with the same selection expected. | PASS before packaging |
| 3 | **Live part, chosen by impact.** For a change the checker sorts every case into **affected**, **no identified dependency (covered by regression tests)** or **impact unknown**, from a small dependency map (source area, capability or provider, cases). An unclassified source change is "impact unknown", never an empty list. The list spends Cort's play time well; it does not prove the other games untouched, and a replay that passes today does not renew a game's older live verification. | The affected cases scored |
| 4 | **First contact.** A new game's first run uses the current build with zero new code and is saved as that game's first result, whatever it shows. | Result saved |
| 5 | **Fix by rule.** The failed job names its ladder; the fix improves or adds a general rule, with a replay case from that game; every other case keeps its floor. | Checker PASS, then the game's next run |
| 6 | **Floors move carefully.** A better run *proposes* a new floor. The old floor stands until that case's next planned run confirms it within the stated variation; no run is requested only to confirm. Floors go down only by Cort's decision. Earlier evidence is kept, never overwritten. | Floor file updated with both runs named |
| 7 | **Number the build.** A build that passes and adds a case or raises a floor is the next baseline; Cort is told the exact hash before he runs it. | Ledger row |

### 0.5 What the checker measures

Receding = a verified job fails, a floor falls, or title-specific code in use rises without Cort's decision. Coverage, gaps and frame time are always shown together, so dropping work can never pass for a speed-up.

| Measure | Notes |
|---|---|
| Each required job's state | Route B from existing counters; Route A scoring is new |
| Route B coverage as three numbers side by side: frames presented, share the program judged to be scene, share with DLAA delivered | They make a drop in recognised scenes visible. They do not establish which unrecognised frames were gameplay, and the report says so. |
| Continuity: the longest gap over observed opportunities | A sampled estimate today and labelled as one. A later exact counter must count defined eligible frames and tell intended Off, menu and loading apart from unexpected misses. |
| Speed: whole-run frame time over the measured play interval, the skipped or missing share, and fps while DLAA was delivering | The conditional number alone can hide bad stretches |
| Time to first delivery, and recovery after a layout change | From the frame-stamped event log |
| Title-specific dependencies actually used | Admission method reported apart from where the layout came from: a shared rule validating a seeded layout is validation with a hint, not discovery |
| Transfer and tested pattern combinations (0.6) | |

Comparisons hold only between comparable evidence: same case and settings, a known producer build, the same kind of evidence. Camera rotation and translation flags (with "unknown" where the pose cannot say) help split a run, but they describe the camera, not the scene: a battle under a still camera still changes cost. An on/off interval in a changing scene is an observed comparison, not an isolated cost of DLAA.

### 0.6 The pattern map: how we know the program is getting broader

"Any game type" means any rendering pattern. Rows are the patterns the program observes and logs; columns are cases; each cell says which shared rule handled it, or which title-specific code was used. The map records **tested combinations** as well as single patterns: standard depth in one game and a relative camera in another do not prove the two together.

| Pattern | Kinds seen so far |
|---|---|
| Graphics API and provider | DX11 (WWII, Pharaoh), DX12 (Anno, Sniper Elite 5, Resistance, Tsushima, PBZ) |
| Camera admission | inverse pair; no inverse (start, continue); camera-relative with the position witnessed elsewhere. Layout seeded or learned is recorded separately. Relative learning is wired to native DX11 only today. |
| Depth | the convention the consumer used, and where that came from |
| Jitter | route and sample; requested, applied and consumed kept apart when they differ |
| Motion | as reported by the motion backend (camera motion plus residual flow is a combination) |
| HUD and presentation | only what the evidence shows; the rest stays unknown |

A new game's zero-change success and an independent engine or pattern are credited separately. Holes in the map choose the next first-contact game.

### 0.7 The run card

Written by the checker before each run and answered by it afterwards: the build hash and how to confirm it (the installer's S option; the first line of the startup log), the one question this run answers, what to do, and what each outcome means for the next step. "What to do" is the case's **standard scene**: one save, about three minutes: still, turning, moving, with DLAA switched off from the Home menu for about 20 seconds. After the run the installer's L option saves the logs. The build is read from the logs themselves; a label typed by anyone cannot rename old evidence.

### 0.8 The build ledger

`docs/BUILD-LEDGER.md`, newest at the bottom, from Baseline 1 on. Each case keeps its last known-good hash, so a regression is answered by comparing two named builds and the rule that changed behaviour between them.

| Baseline | Build hash | Date | Cases run by Cort | Jobs verified | Floors proposed or confirmed | Title-specific code in use | What changed for the player |
|---|---|---|---|---|---|---|---|

### 0.9 Milestones (proposed; Cort decides)

| Milestone | Done when |
|---|---|
| **M1: Baseline 1** | The six retest games scored, floors proposed, ledger row written |
| **M2: First proof of transfer** | Pharaoh reaches every Route B job verified with no title-specific code in use |
| **M3: Broad** | Three new games in a row, on different engines, reach every required job with no new title-specific code. A worthwhile mark, not an estimate of how many games work. |

### 0.10 Build plan for the growth checker (Cort, 02:49 PT)

**Form.** A command-line tool in Python 3 using only the standard library, in `tools/growth_checker/`, with its own offline tests. Nothing is installed on Cort's PC: it runs on the logs he saves. It produces three things: a result file per case (numbers with their sources), one self-contained report page for Cort, and a proposed ledger row.

| Where | What lives there |
|---|---|
| `tools/growth_checker/` | The tool: `schema.md`, readers, scoring, `rules/` (Astra), report, `tests/` |
| `growth/cases/` | One file per case: game, route, API, settings, standard scene, required jobs, budget |
| `growth/results/<build>/` | Scored results with their provenance. Added to, never overwritten. Raw logs are not committed; a result names its log files by hash. |
| `growth/floors/` | The floor per case with its history and any open proposal |
| `growth/dependencies.json` | The dependency map behind the impact list |
| `docs/BUILD-LEDGER.md` | One row per baseline |

**Commands:** `ingest` (a saved run folder becomes a manifest and a scored result), `compare` (a build against the baseline, case by case), `report` (the page and the ledger row), `impact` (which cases a change can affect), `selftest`.

**Claude's tasks**

| # | Task | Done when |
|---|---|---|
| G1 | **Data format first** (`schema.md`, sample records): evidence manifest with producer build read from the log, run and session identity, case, settings, API, evidence type, scorer and schema version, file hashes | Published so Astra's rules can be written against it |
| G2 | **Readers** for native DX11, DX12, Route A and the event log. The run is chosen by its build line and session, never by the largest log. Sessions split at a counter reset. A missing value is "unknown", never zero. A parse failure is its own status, never a pass. | Offline tests on the existing sample and synthetic logs |
| G3 | **Cases and job scoring** per route, with the per-case budget | The six retest cases defined |
| G4 | **Results store and floors**: append-only results, floor proposals and confirmations as Astra's rules decide | A proposal cannot replace a floor by itself |
| G5 | **Report page and ledger row**: route-aware matrix, each case against its baseline, trend, unknowns, warnings, links to the evidence | One self-contained page |
| G6 | **Impact list** from the dependency map, with its three answers | "Impact unknown" for an unclassified change |
| G7 | **Command, offline tests, CI step.** The tool is tested on intentionally wrong results before its green is trusted: wrong-build evidence, a missing required maximum, a counter reset, a candidate that recognises fewer scenes, a change to a previously rejected rule, a shared dependency reaching two providers; plus a correctly unchanged and a correctly improved case. | All eight give the expected answer |
| G8 | **Compile**: bring in Astra's rule modules, run everything, score the retest logs, deliver the first report and the Baseline 1 proposal to Cort | Report in Cort's hands |
| G9 | *Later, a NeuralAlias change with its own PR, never for `b8b6a4a`:* pattern and pose fields at the existing scene handoff, as Astra specified in COMMS (a) and (b) | Next package after the retest |

**Astra's tasks: the judgment rules.** Each is delivered as a decision table (`rules/README.md`), a pure function over the records of `schema.md` (no file or network access), and test vectors with the expected answer (`tests/vectors/<rule>/`). Claude wires them in at G8.

| # | Task | The question it settles |
|---|---|---|
| A1 | **Valid comparison** (`rules/comparison.py`) | When may two results be compared? Which identity and settings must match, how warm-up, cuts, restarts and intended Off or menu intervals are treated, how much evidence is enough, what variation is normal, and when a better sample may propose a floor and what confirms it. Answers: comparable, comparable with stated limits, not comparable (with the reason). |
| A2 | **Misleading results** (`rules/guards.py`) | Which results look better without being better? Fewer scenes recognised, a speed gain beside lost coverage, a conditional fps hiding bad stretches, a missing value passing a limit, a counter reset, a relabelled or re-scored old log shown as new, a sampled gap shown as exact. Each guard names its inputs, its trigger and the sentence the report prints. |
| A3 | **Transfer across games** (`rules/transfer.py`) | What earns credit for getting broader? Zero-change success, an independent engine or pattern, a tested combination, and hint against discovery, each counted apart; and how title-specific dependencies in use are counted, Route A adapters and shared files included. |
| A4 | **Ambiguous evidence** (`rules/ambiguity.py`) | What does the checker say when evidence is missing, partial or in conflict? Live run against replay against fixture, an old live verification beside a new replay pass, two runs that disagree, a telemetry gap; how "unknown" travels from a job to a case to the build's overall answer, and the words for that answer. |
| A5 | **Review of the dependency map** (small) | Does the map of G6 cover selection and priority logic, eligible alternatives and common dependencies? |

**Sol:** merges the checker's PR into the integration branch; writes the ledger row at packaging; in the next package the installer's L option also writes a small run manifest (build, profile, session, file hashes), so provenance is fixed where the logs are saved.

**Order.** G1 goes first because Astra's rules are written against it. Then A1 to A4 and G2 to G7 proceed side by side. Cort's retest waits for none of it. G8 follows when both halves and the retest logs exist. G9 and the scorecard page fed by the report come after.

---

## 1. How we work (read this first)

**Cort's project-wide direction (2026-11-01):** avoid relentless skepticism or looking for something to object to. Give a sound idea plain credit and pursue it. Raise concrete defects and material limitations when supported by evidence; do not turn hypothetical concerns into extra gates, scope or review cycles. This applies for the duration of the project.

| Rule | Detail |
|---|---|
| **Build mode now** | Cort's direction: **both agents build, nobody reviews yet.** Don't block on the other agent. Don't edit the other agent's files. |
| **Review gate sequence** | Starting at Gate 2: **Claude reviews every Astra PR; Astra reviews every Claude PR**. Authors resolve findings and run regressions, then **Gemini Argon audits the full gate snapshot and surrounding architecture**. Resolve/recheck findings, then run the scheduled Windows/GPU/game session and close the gate. Routine developer tests and CI continue throughout. |
| **Order** | Work your tasks **top to bottom**. Phases go from *already proven possible* → *hard*. Don't start a later phase's task while an earlier task of yours is open, unless it's blocked (say so in COMMS.md). |
| **Branches** | Claude → `claude/<task-id>-<topic>`, Astra → `astra/<task-id>-<topic>`. One branch per task. Commit and push when each task is done. Open a PR into `main` (it waits for the review gate). |
| **Coordination files on `main`** | `MASTER-PLAN.md`, `COMMS.md`, `COLLABORATION.md` and `HANDOFF.md` may be edited **directly on `main`** (pull/rebase first, small commits, never force-push). Everything else goes through a branch + PR. |
| **Talking to each other** | `COMMS.md` only. Append-only, newest at the bottom. Questions, blockers, done notices, interface requests and Argon handoff/audit status. Argon receives a commit-pinned review packet after cross-review; see COLLABORATION.md. |
| **Shared interfaces** | ABI 0.1 (`host_observe.h`) and `src/core/` stay frozen. Changes need a numbered proposal in COLLABORATION.md plus an ack from the other agent in COMMS.md. |
| **Never commit** | NVIDIA SDK headers/libs/DLLs, game files, captures, `local/`. |
| **Done means** | The task's "Done when" line is met on CI (Linux/Windows build) **or** clearly marked "needs Windows GPU" for the test session. No placeholder tests that pass by doing nothing. |

### File ownership

| Path | Owner |
|---|---|
| `CMakeLists.txt` (root), `build.ps1`, `.github/workflows/` | **Claude** (single editor) |
| `src/host_dx12/**`, `src/runtime/**`, `src/app/**`, `tools/**`, `scripts/deploy-*` | **Claude** |
| `tests/host/**` (GPU/host/proxy tests + stubs) | **Claude** |
| `src/providers/discovery/**`, `src/providers/classifier/**`, `src/providers/camera/**`, `profiles/**` | **Astra**, except the two files below |
| `src/providers/camera/matrix_candidates.hpp`, `src/providers/discovery/diagnostic_adapter.cpp` (and their tests) | **Claude** (transferred 2026-10-25 by Cort for the drop-in algorithms, C12/C13) |
| `tests/cpu/**` including `tests/cpu/CMakeLists.txt` (pulled in by root via `add_subdirectory`) | **Astra** |
| `tests/fixture/**` (ground-truth scene) | **Astra** |
| `src/core/**`, `host_observe.h` | **Frozen. Joint proposal required** |
| `docs/` | Each agent edits its own docs; shared docs by proposal |

---

> Note (2026-11-18): rows above about review gates, Gemini Argon, "one branch per task" and PRs into `main` are the October set-up. Current practice: build now, the other agent audits at Cort's request, no extra review gates, no branch sprawl (work converges on the active line named in COMMS "Current state"). Cort's rules in COMMS and COLLABORATION.md win where they differ.

## 3. Environment (answered by Cort 2026-10-23)
- **NVIDIA SDKs:** all present on Cort's PC at `C:\Users\Cort\Documents\NeuralAliasAbundance-SDKs\`: `nvidia-dlss-sdk\` (NGX 310.9.16 headers + `nvsdk_ngx_s.lib`), `nvapi\`, `pclstats\`, `optical-flow\` (OF SDK 5.0.7 headers incl. `nvOpticalFlowD3D12.h`). DLSS runtime `nvngx_dlss.dll` 310.9.16 lives in `nvidia-dlss-sdk\lib\Windows_x86_64\rel\`. Everything project-related stays inside this one folder. **C1 makes these the default search paths** (overridable by cache variables); missing = feature compiled out.
- **Test games:** TLOU Part I (native DLSS) and Anno 1800 (none). Cort's DLSS games also expose FSR/XeSS, so they cover Phase 2 FSR/XeSS testing.
- **Windows host:** Cort's desktop (RTX 5070 Ti). Visual Studio and Git are **installed** (2026-10-23).


## Final builds per game (Cort, 2026-11-13 07:01Z)

Cort: "Of each of those games, the final version declared to be workable should be labelled final, and merged, every version up to that point should be removed."

| Game | Final build | Label to create | Evidence | In the code line (56e49ef)? |
|---|---|---|---|---|
| Anno 1800 | e7529c3 | `final/anno-1800` | GameCheck verified; Cort: "Sol fixed Anno" | yes |
| Sniper Elite 5 | 446a274 | `final/sniper-elite-5` | about 2 h, DLAA on 99.89% of frames | yes |
| Sniper Elite Resistance | 446a274 | `final/sniper-elite-resistance` | 99.6% of game frames | yes |
| Call of Duty WWII | fc0e6bd | `final/cod-wwii` | 96.3%, GameCheck verified | yes |
| Battlefield 4 | none yet | | 56e49ef staged, not run | |
| Hearts of Iron IV | none yet | | 496d691 staged, not run | |

Rule from now on: when a game gets a newer workable build, this row moves to it, the label moves, and the earlier packages for that game are removed. The finals are not yet on the consolidation branch or `main` (446a274 and e7529c3 sit on the line above it); merging them there is Sol's step as merger.
