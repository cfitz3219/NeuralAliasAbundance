# J2 moving-object motion: optical flow — plan for Astra/Sol approval

Revision 1 — 2026-10-31. Author: Claude (Opus), at Cort's request.

**Status: PROPOSED. Approval requested from Astra/Sol through COMMS.** No runtime source, build, package, game file or running game was changed to write this plan. Baseline: origin/main 0f96c4d.

## 1. Goal, in plain language

J2 (live DLAA for DX12 games that ship no DLSS and no TAA, e.g. Anno 1800, Sniper Elite Resistance/5) gives DLSS motion only from camera + depth. That is correct for still scenery (Anno's stationary blur was fixed by the double-composed reprojection, 35bea07), but wrong for anything that moves on its own: units, ships, people, animals, cloth, foliage and, in third-person games, the player character the camera orbits. DLSS then drags history behind those objects (ghosting, soft trails).

J2 target games have no TAA and therefore, in general, no engine motion-vector buffer (HANDOFF.md: TAA games are out of scope for J2). The general source for object motion is therefore image-based: NVIDIA hardware optical flow (NVOFA), already wrapped in the codebase. This plan wires it into J2 correctly and adds DLSS's bias-current-colour mask as the safety net where flow is not trusted.

Out of scope: TAA games, render-resolution control (J2 stays DLAA), DX11/Vulkan, shader instrumentation of per-object transforms, NR/FG ownership (O2), FSR/XeSS replacement.

## 2. What already exists (reuse, do not rebuild)

| Piece | Where | State |
|---|---|---|
| NVOFA wrapper, async fences, teardown rule | src/host_dx12/nvof/nvof_flow.* | Built and tested (A36); BGRA8 display-encoded inputs; output grid R16G16 S10.5 |
| Grid → full-res flow, depth-aware, validity at disocclusions | src/host_dx12/nvof/flow_expand.* | Built; validity must be consumed |
| Live supplier at Present (+ PQ/SDR encode) | src/host_dx12/motion/live_flow.* | Built, off by default ([motion] enabled=1); runs on the FINISHED frame (post-HUD, one frame late) |
| Live flow accuracy vs engine MVs in DLSS games | live_flow EvalRef / Stats | Built: mean endpoint error and % within 1 px against the game's own motion vectors |
| Arbiter tiers 1/2/3, residual derivation, provenance | src/core/motion_arbiter*.{hpp,cpp}, motion_arbiter_gpu.* | Tier 3 (residual flow + mask + validity) implemented, unused by J2 |
| J2 camera motion | src/host_dx12/j2/live_dlaa.cpp ~1088 | Tier 2 only |
| DLSS bias mask parameter | src/host_dx12/dlss/dlss_backend.cpp:670 | Passed as nullptr today |

## 3. The design problem to settle first (D1)

J2 currently inserts DLSS **inside the game's own command list** in the target games (SER/SE5 status: `route=inline … "Post-processing is recorded in the scene's own list: insertion inside that list."`). NVOFA executes as a separate submission synchronised by fences; it cannot run in the middle of a command list the game has not yet submitted. Simply consuming live_flow's output would use the previous, HUD-composited frame: one frame stale and HUD-contaminated, which itself causes smearing.

Options, to be decided in review before coding:

- **D1-A Flow on the previous scene pair, applied with one frame latency.** At J2's inline point, keep a copy of scene colour; after the game's list is submitted, run NVOFA on (N, N−1) pre-HUD scene copies; frame N+1 uses that residual. Cheap to integrate, works with inline. Assumes object motion is roughly constant over one frame: good for ships/units/strafing characters, weaker for fast animation and starts/stops.
- **D1-B Same-frame NVOFA on list-boundary routes.** Where J2 inserts at a list boundary instead of inline, signal a fence after the scene list, run NVOFA, make the DLSS list wait on NVOFA's fence (GPU→GPU only). Exact frame, but unavailable on inline routes, which are the ones observed in the target games.
- **D1-C Our own compute-shader flow recorded inline.** A coarse pyramid block-match / Lucas-Kanade at quarter resolution, recorded directly in the game's list before DLSS. Same frame, works inline, no NVOFA dependency (non-NVIDIA-OFA-safe), but new code and lower accuracy than hardware flow.

**Proposed:** D1-A first (smallest change that helps the target games now), with D1-B used automatically when a game's route is a list boundary. D1-C only if D1-A measures poorly on animation. Reviewer to confirm or redirect.

## 4. Work packages

**F0 — Measure before building (about 0.5 day, mostly automated).** Use live_flow's existing EvalRef accuracy mode in a DLSS game where ground truth exists (Ghost or PBZ, motion enabled, J2 off) to record NVOFA endpoint error and % within 1 px at grid 4/perf 5, grid 4/perf 20 and grid 2, plus GPU time. This gives a real accuracy/cost table before J2 depends on it. Needs one Cort session; no new code beyond a status line if missing.

**F1 — Pre-HUD scene flow inside J2 (about 1–1.5 days).**
- At the J2 insertion point, copy scene colour (already available) into a small ring, encode to BGRA8 with the exposure J2 already uses for DLSS (reuse live_flow's encode path; log/exposure encode for HDR scene formats).
- Run NVOFA current→previous on that ring per D1, on the queue J2 already uses, with existing fence rules; no CPU waits, skip-and-count when the ring slot is busy (live_flow rule).
- Expand with FlowExpand depth-aware using J2's depth32 copy; validity is mandatory.
- Respect the NVOFA teardown rule and resize: re-init on scene-size change, unregister before release.

**F2 — Arbiter tier 3 in J2 (about 0.5–1 day).**
- Derive the residual with the arbiter's existing DispatchDeriveResidual (raw flow minus camera motion, folding FlowExpand validity in), then feed Tier3 into the existing Dispatch call next to Tier 2. Keep residual_snap_px/residual_max_px defaults; residual applies only where camera motion is valid (existing contract).
- Under D1-A, scale/shift the one-frame-late residual explicitly and record that provenance; never label it same-frame.
- Status: per-frame counts of residual-added/snapped/clamped pixels, flow age (frames), skipped flow frames, GPU ms.

**F3 — Bias-current-colour mask (about 0.5 day).**
- Build an R8 mask where flow validity is 0, residual was clamped, or camera and flow disagree strongly; pass it instead of nullptr in dlss_backend.cpp. Effect: DLSS trusts the current frame there instead of history: less ghosting where motion is uncertain, at a small loss of temporal stability.
- Useful even before F1 lands (mask from depth disocclusion alone), so it can ship first.

**F4 — Controls, tests, package (about 0.5–1 day).**
- Menu: "Moving-object motion" Off / On (default Off until Cort's live check), optional quality (fast/balanced). Saved through existing settings ownership (respect O3).
- Tests: synthetic GPU scene with a moving object over a panning background (WARP/RTX), asserting residual applied on the object, zero on static background, validity 0 at disocclusion edges, mask set there; teardown/resize with registered NVOFA resources; flow-busy skip path. Existing j2_live/j2_anno_shape/arbiter tests must still pass.
- Candidate package for Anno and Sniper Elite Resistance only (existing NA-anno-dlaa / NA-ser-dlaa launchers), separate from Ghost/PBZ candidates.

Total: roughly 3–4.5 engineering days after D1 is settled, plus two Cort sessions (F0 in a DLSS game; final check in Anno + SER).

## 5. Acceptance (Cort's live checks)

- Anno: ships and units moving across a still camera: no trails; still scenery stays as sharp as today.
- Sniper Elite Resistance: strafe/orbit the camera around the character: character stays sharp; NPCs walking.
- Frame time cost reported in the status line; target ≤ 3 ms at 3440×1440 on the RTX 5070 Ti, else use a faster flow setting.
- Off returns exactly to today's J2 behaviour.

## 6. Risks

- Flow fails on water, smoke, particles, glass and flat colour: handled by validity + bias mask, not by trusting flow; may need per-game tuning of disagreement thresholds.
- D1-A latency on fast animation; mitigated by the mask; D1-C fallback.
- NVOFA availability: RTX 20+ only; feature reports "unavailable" and J2 behaves as today elsewhere.
- GPU cost lowers fps; measured in F0 and shown in status.
- J2 engagement drop-outs (Anno no_scene_list/no_selection; SE5 low coverage) are a separate reliability problem; this plan does not fix them, and they should be fixed first or alongside so flow is evaluated on frames J2 actually processes.

## 7. Ownership and coordination

Proposed implementer: Astra (owns J2/production worktree) or Opus if Astra prefers; reviewer the other. No parallel edits in live_dlaa.cpp, motion_arbiter*, nvof/*, dlss_backend.cpp. Commit independently reviewable increments (F3 first, then F1+F2, then F4). No game launch/control/deployment by agents; Cort runs the live checks.

## Review log

- Rev 1 by Claude (Opus). Approval requested from Astra/Sol in COMMS. Not approved yet.
