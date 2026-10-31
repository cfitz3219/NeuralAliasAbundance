# P4-0b: moving-object motion for live DLAA (J2) with NVIDIA optical flow

**Status:** PROPOSAL for approval by Astra (and Sol, at Cort's request). No code written. Author: Claude/Opus, 2026-10-31, at Cort's direction ("Create a plan of action. Then submit it for Astra/Sol's approval").
**Base:** `astra/pbz-rebuild-queues` at b2806ff (PR #59). MASTER-PLAN Phase 4, task P4-0(b): "Motion for moving objects ... Reuse NVOF / `motion/live_flow` on the pre-HUD scene image with per-pixel provenance."

## 1. Problem

J2 builds motion vectors from camera reprojection only (`j2/live_dlaa.cpp`, the arbiter dispatch with `tier2_valid_flag = true` and no tier 1 or tier 3). Anything that moves relative to the world (ships, carts, people, foliage, and in third-person games the player character) gets the camera's motion, so DLSS reuses the wrong history there: trails, ghosting, smear. J2 targets games without TAA (HANDOFF: TAA games are out of scope), and those games do not render a velocity buffer, so optical flow is the motion source. It is also the prerequisite for P4-2 (Frame Generation on this route).

## 2. What already exists (reuse, no new solver)

| Piece | File | State |
|---|---|---|
| NVOFA wrapper (Optical Flow SDK 5.0, D3D12, fence-based, teardown rule A36) | `src/host_dx12/nvof/nvof_flow.{hpp,cpp}` | Built, used by live_flow |
| Grid to full-res expansion, depth-aware, writes a validity mask | `src/host_dx12/nvof/flow_expand.{hpp,cpp}` | Built |
| Arbiter tier 3 (residual + mask + validity) and `DispatchDeriveResidual` (raw flow minus camera motion minus jitter delta) | `src/core/motion_arbiter_gpu.{hpp,cpp}` | Built; J2 already owns an `Arbiter` |
| Live flow at Present (HDR/SDR to BGRA8 encode, ring, no render-thread waits, latch-off) | `src/host_dx12/motion/live_flow.{hpp,cpp}` | Built, off by default; measured ~4 ms at 3440x1440 grid 4 / perf 5 |

**Why live_flow cannot simply be plugged in:** it runs at Present on the finished back buffer, so its flow is one frame late and contains the HUD and tonemapping. J2 needs flow for the *current* frame, on the pre-HUD scene colour, before DLSS evaluates.

## 3. The constraint that shapes the design

NVOFA is not recorded into a command list. `Flow::Execute` is a queue-level operation: it waits on a fence and signals another. So "current-frame flow before DLSS" needs a fence between "scene colour copied" and "DLSS evaluate". That is possible on one J2 route and not on the other:

| J2 route | Games | Same-frame NVOFA possible? |
|---|---|---|
| **Between lists** (J2 records its own list after the game's last scene list) | Anno 1800 | **Yes.** Split the ExecuteCommandLists batch at our list: game lists + our copy/encode list, `Signal`, NVOFA `Execute` (waits, signals), `queue->Wait(flow fence)`, then our DLSS list + the rest of the batch. GPU-side waits only; same queue order preserved. |
| **Inline** (J2 records inside the game's list, at the scene-colour consumer) | Sniper Elite Resistance, Sniper Elite 5 | **No.** The game's own post-processing follows in the same list, so no fence can be placed between our copy and our evaluate. |

Cost note for the between-lists split: the direct queue idles while NVOFA runs unless other work overlaps, so the 2–4 ms is on the frame's critical path. Grid 4 with perf 20 (fast) and half-resolution input are the first things to measure.

## 4. Plan

### Step 0 — evidence first (no code, one short session each, Cort)
- Sniper Elite Resistance: strafe the camera around the player character with the motion debug view on, current J2 build. Confirms how bad the trailing is and gives a before image.
- Anno: same with a moving ship. Doubles as the owed Anno re-test on the current build.
These are the baseline screenshots the acceptance criteria compare against.

### Step 1 — bias-current-colour mask (½ day, both routes, cheapest visible win)
- Add `bias_current_color` (R8_UNORM, optional) to `na::dlss::EvalDesc` and pass it as `NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask`.
- J2 fills it from a cheap compute pass: pixels where the reprojected previous colour disagrees strongly with the current colour (luma difference after exposure normalisation), excluding sky. DLSS then trusts the current frame there instead of smearing history.
- This reduces ghosting without making movers sharp. It stays as the safety net under flow in steps 2 and 3.
- Test: host test with a synthetic moving square; mask covers the square's leading and trailing edge, zero on a still scene.

### Step 2 — same-frame NVOFA on the between-lists route (2–3 days, Anno first)
1. **Encode.** After J2's existing colour copy, a compute pass writes the pre-HUD scene colour to a BGRA8 texture using one fixed exposure for both frames of a pair (previous frame's average luminance), Reinhard + sRGB. Reuse live_flow's encode shader structure. Two BGRA8 textures in a ring (current/previous), registered with NVOFA once.
2. **Flow.** Split the batch as in §3. `Execute(current, previous, grid_out, ...)` with temporal hints disabled on resets/cuts.
3. **Expand.** `flow_expand` depth-aware mode with J2's `depth32` (raw depth, correct clear value for reversed-Z) to full-res RG32F raw flow + validity.
4. **Fuse.** `Arbiter::DispatchDeriveResidual(raw_flow, depth, camera, jitter_prev, residual, valid, raw_valid = expand validity)`, then the existing arbiter `Dispatch` with tier 2 + tier 3. Camera motion stays authoritative on static geometry; the residual is added only where flow and validity say the pixel moved beyond the camera. Arbiter snap/clamp rules unchanged.
5. **Mask.** Low tier-3 validity or disocclusion feeds the step-1 bias mask.
6. **Controls.** `[j2] flow=0` default (off until qualified), menu switch, status counters `flow= frames/skipped/latched ms_gpu residual_px_mean coverage%`. Any NVOFA failure latches flow off with a plain reason and J2 keeps running with camera motion (never stops DLAA).
7. **Lifetime.** Honour the A36 teardown rule (unregister + release before `Shutdown`, never unload nvofapi64.dll). Resize/route change: unregister, rebuild, reset temporal hints. No CPU waits anywhere.

### Step 3 — inline route (Sniper): choose one, decided before coding
Same-frame NVOFA is impossible inline (§3). Options:
- **3A. One-frame-late NVOFA residual (recommended to try first, 1 day on top of step 2).** Compute flow for the pair (N-1, N-2) asynchronously off the critical path, warp the residual forward by itself to frame N (constant-velocity assumption), and gate it with validity *and* the step-1 disagreement mask. Correct for steady motion (walking, strafing, ships), wrong for sudden starts and stops, which the bias mask covers. No queue stall at all, so cheaper than step 2.
- **3B. FidelityFX optical flow (MIT, the compute-shader flow inside FSR 3 frame interpolation), recorded in the game's list.** Same-frame and route-independent, about 1 ms at reduced resolution on high-end GPUs per AMD. It is an established implementation, not our own solver, but it is a new import (pin revision, keep notices) and needs Cort's OK under the reuse rule.
- Recommendation: build 3A, measure on SER; adopt 3B only if 3A's start/stop errors are visible in Cort's test.

### Step 4 — tests (CI) and live acceptance
- CI (WARP, no NVOFA): batch-split ordering test with a stub flow engine (signal order, queue waits, no CPU wait, failure latches off, resize/teardown rule); arbiter tier-2 + tier-3 fusion test with a synthetic moving object over a moving camera (residual only on the object, zero on static pixels). Restore the arbiter CPU-vs-GPU parity test the header refers to (`motion_arbiter_gpu_test` is not in the tree).
- GPU-labelled test (real NVOFA, Cort's install script): flow on two rendered frames of a translated square returns the known displacement within 1 px.
- Live, Anno then SER, one session each:
  - Motion debug view shows motion on ships/people/the player character; static scenery unchanged against Step 0.
  - Visibly no trail behind moving objects with DLAA on, against Step 0 screenshots.
  - Measured cost in the status line; target ≤ 3 ms at 3440x1440, else grid/perf/half-res is tuned before shipping.
  - Toggle flow off/on, resize, alt-tab, exit: clean, no device removal.

## 5. Ownership, order, estimate

- MASTER-PLAN P4-0b: **Opus implements, Astra reviews.** J2 production placement is Astra's lane (COMMS 2026-10-28); this plan touches `live_dlaa.cpp` only at the record path (after the colour/depth copy, before `na::dlss::Evaluate`) and the between-lists submit. Astra, please say whether you want those hunks or prefer to own them.
- New files (Opus): `src/host_dx12/j2/j2_flow.{hpp,cpp}` (encode, NVOFA ring, expand, derive, bias mask). `dlss_backend` gets one optional input.
- Order: after the ownership follow-up (stage-2 F1 + Cort's Apply-only decision) and Step 0. Then Step 1 → Step 2 → Step 3A → tests → live.
- Estimate: Step 1 ½ day, Step 2 2–3 days, Step 3A 1 day, tests 1 day, plus Cort's two live sessions.

## 6. Questions for Astra / Sol

1. Batch split for the between-lists route: any objection to splitting the game's ExecuteCommandLists call at our list (same queue, order preserved), given the router and `count_post_dlaa` logic that run on that batch?
2. Inline: agree to try 3A first, or go straight to 3B?
3. Exposure for the BGRA8 encode: fixed per pair from the previous frame's mean luminance, or something you have already qualified in live_flow/NR?
4. Anything in the A36/A52 NVOFA notes I should know that is not in the headers?
