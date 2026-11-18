# HANDOFF

Short version (2026-11-18). For the current state of the work (owners, active branch and PRs, what is on Cort's PC, open items, rules), read the **"Current state" section at the top of COMMS.md**. This file keeps only the latest handoff notes.

The full previous journal (1,177 lines, 213 KB, 2026-09 to 2026-11-17: milestone 1, gates, Anno, Sniper, PBZ, Ghost, WWII, BF4, HOI4, Rome II, materials, camera-independent DLAA) is kept word for word in [`docs/comms-archive/HANDOFF-through-2026-11-18.md`](docs/comms-archive/HANDOFF-through-2026-11-18.md).

**Keeping it short:** add a few lines per milestone (what is ready, where, what it waits on). When this file passes ~20 KB, move the older notes verbatim to the end of a file in `docs/comms-archive/`.

## Latest handoff notes (verbatim)

### 2026-11-17 — GitHub backlog and main consolidation complete

Reviewed all 39 original open PRs. Merged #133, #132, #134, #135 and #144 into Route B, then merged #158 to main as 82fe548 after all four GitHub jobs passed. That brings the consolidated DX11/DX12 source and the latest GameCheck rules into main. GameCheck passed 38 tests locally. Closed 24 obsolete, duplicate, or carried-forward PRs after verifying their disposition. The remaining ten are #83, #88–#94, #142 and #145. Earlier branches are retained. Fable's active Resistance build and follow-up are still tracked by #145. See docs/PR-BACKLOG-20261117.md for the evidence and decision on each original PR.

### 2026-11-17 — Referee stage 1 patched

PR #159 at a867810 targets Fable's active branch. Exact-frame and swap-chain matching, independent fences, bounded recording lifetime, actual NGX snapshots, separate submission coverage, lower DX11 copy load, and off-thread scoring are implemented. Release host and five focused checks passed, plus WARP. COMMS has the integration notes. The referee is opt-in pending real-game calibration. No package or installed game changed.

PR #159 merged to the active branch at 45441f2. The merged tree is identical to the tested tree. Saved-capture calibration and wiring the verdicts to the route selector are still separate work.

PR #161 merged at 3536dbba. It connects the referee to a read-only route preview and replays saved Resistance captures through the same policy. It does not give the referee control of the live route. COMMS and the active WORKLOG record the samples, controlled bypass and limits of the evidence.

## 2026-11-17 Pacific — Native DX11 jitter-locked lighting handoff

Astra's native DX11 implementation is a561d9e (PR #162 into Fable's claude/project-thread-axt148). Fable retains the DX12 implementation and combined integration. The Release host built, hardware and WARP passed the depth-slab fixture, and the debug-wrapper fallback was verified. Original depth tests and DLAA inputs are intact. COMMS records the rendering condition, state-handling decision and game limits. Source handoff only. No installation, package or live game run.

## 2026-11-17 Pacific — Cross-review of depth correction

Cort requested a reciprocal review. Astra asked Fable in COMMS to review DX11 PR #162 at a561d9e (12/12 Release tests). Astra reviewed Fable's DX12 2db2250. The original fixture and shader checks passed on the RTX, but narrow hardware/WARP probes reproduced premature twin activation, descriptor semantics changes, residual viewport-expansion error, and injected query/work duplication. COMMS carries the measurements, DX12 corrections and local reproduction path. Fable owns DX12 fixes. No deployment or gameplay test.


## 2026-11-17 Pacific — Adaptive referee audit complete on isolated draft

Astra's draft PR #163 (5a84308) targets Fable's active code line. It improves DX12 placement discovery when the final scene writer changes and lets the shared referee relocate its picture comparisons, use isolated or padded evaluations and distinguish intermittent delivery. Thirteen automated regressions passed, including native DX11 and DX12. The referee ran on the RTX 5070 Ti and WARP. Two saved Resistance runs gave 60/60 output-favoring screens, and a controlled alternating replay flagged mixed delivery. The draft's WORKLOG records limits and reproduction evidence. Cort's false-motion hold still applies to merging and packaging.

## 2026-11-17 Pacific — Native DX12 screen-check handoff

Sol's draft PR #164 (b71fa1b, base 0cb1fb46) reuses the scale bridge's screen probe to verify native DLAA placement. RTX 5070 Ti rejected the decoy and confirmed the real reader, with 29 consecutive presented frames carrying the test evaluator's output through the game's own post-processing. The Release host and all 16 focused checks passed. The new fixture also passed on WARP. Fable owns integration and packaging. Shadow of War's DX11 route is outside the new path.

## 2026-11-17 Pacific — Native DX11 screen-check handoff

PR #165 (89ae5f5 on 8e2eb95b) applies the shared screen check to the native DX11 provider. RTX 5070 Ti and WARP each rejected the decoy, confirmed the visible reader and matched presented pixels on 53 consecutive native-size frames. Eleven selected local cases passed, including two separate native DX11 tests using real NGX. Fable owns integration and packaging. Shadow of War still needs its trial.

## 2026-11-18 Pacific — Shadow of War DX11 Referee continuity

PR 166 (07acf67) targets the active 78b539df integration line. The native DX11 Referee now preserves ordinary DLAA delivery and requires a candidate to stay present throughout screen proof. A reader that later alternates between delivery and misses loses control. Ten focused tests passed. RTX and WARP pixel checks passed for ordinary delivery, interrupted proof, discovery of the visible reader and restoration after alternating misses. No live game run or installer. The separate overhead from camera discovery while DLAA is paused is documented in the branch's WORKLOG.

## 2026-11-18 Pacific — Shadow of War final-image monitoring and camera cost

PR 166 ends at c46e5b9 on the active native DX11 base. Referee checks the exact presented frame after approval, aligns the scene and display grids, and retires only the matching reader after repeated clear input votes. Pausing DLAA stops camera acquisition and DLAA-only sampler bias. Optional camera search backs off during successful measured-motion delivery and resumes automatically. Release and 13 focused automated tests passed including native DX12 screen coverage and three real NVIDIA DX11 cases. The classifier passed separately on WARP. COMMS and the branch WORKLOG preserve receipts. No new installer or matched in-game timing on this build.
