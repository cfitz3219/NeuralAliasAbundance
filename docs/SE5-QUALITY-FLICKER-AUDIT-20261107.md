# Sniper Elite 5 Quality: why it still flickers (run p36948, build abb6e12) — for Astra's audit

Run: `Documents/NA-se5-results-20261107-063832/status-36948.log` (manifest sourceCommit `abb6e1223f3645ef6bcc4e13e35c79a85c460ff4`), 06:34:48–06:38:23, ~18,000 presents at 67–112 fps, scene 2293×960 → 3440×1440. Quality was on the whole time. Cort reports "random rapid flickering".

Headline numbers (last `native_delivery` / `native_steps` / `j2_cuts` / `j2_coverage` lines):

| counter | value | meaning |
|---|---|---|
| matched / recorded / submitted | 17468 / 16768 / 16768 | learned dispatch seen 17,468 frames; DLSS written 16,768 |
| best_streak / final streak | 2968 / 648 | longest unbroken run of delivered frames |
| gate drops (misses after a delivery) | 618 | frames where the step ran and no DLSS was written |
| refusal "No unused executable or submitted scene producer matches this upscale pass" | 496 | |
| refusal "The scene selection or producer changed before the upscale recording was submitted" | 119 | |
| ngx_failed | 85 | |
| camera_cut restart_resume | 683 | DLSS history restarts |
| j2_coverage covered | 16988 | all J2 evaluations; 16988 − 16768 = 220 evaluations that were not the Quality route |
| later_written | 3335 | deliveries whose canvas was written again later in the same recording |
| snap{closes/depth_unknown/no_slot/taken} | 85963 / 37419 / 0 / 48046 | depth snapshots at scene-list Close |
| probe | 47 sessions, verdict confirmed_in_presented | DLSS output reaches the presented image when delivered |

496 + 119 = 615 ≈ 618 drops: the two producer refusals account for the misses.

## Per-300-frame deltas (the shape of the problem)

Columns: frame bucket, delivered, drops, later_written, ngx_failed, the two refusals, restart_resume cuts.

```
 3000  300   0    0   0    0   0  |  17
 3300  260  40    0   0   33   7  |  44
 3600  235  65    0   0   53  12  |  77
 3900  256  44    0   0   30  14  |   0
 4200–6600: 300 delivered every bucket, 0 drops, 0 cuts
 6900  290  10    0   0    7   3  |  43
 7200  231  69    0   0   54  15  |  44
 7500  224  76    0   0   65  11  |  48
 7800  297   3    0   0    3   0  |   0
 8100–10500: 300 delivered every bucket (ngx_failed 1/2/6 at 9900–10500), cuts 1–20
10800  254  45    0   1   34  11  |  43
11100  226  74    0   0   65   9  |  99
11400  256  44    0   0   41   3  |   6
11700  297   3    0   0    3   0  |   2
12900  300   0    0   0    0   0  |  36   (25 ngx_failed in the 13200 bucket)
13500  223   0  223  47    0   0  |   2
13800–16800: 300 delivered, 0 drops, later_written == delivered, cuts 0–4
17100  267   3  234   0    3   0  |  51
17400  283  17    0   0   16   1  |  34
17700  225  75    0   0   58  17  |  85
18000  253  47    0   0   31  16  |   0
```

Three regimes: (a) clean stretches, 300/300, no cuts; (b) burst windows of ~900 frames every ~3,600–3,900 frames (3300–3900, 6900–7800, 10800–11700, 17400–18000) with ~25 % misses and 40–100 history restarts; (c) one phase (13500–17100) where every delivery has `later_written=1` and almost no misses or restarts.

## Contender 1 — the scene producer is stale when the upscale is recorded, in periodic bursts (primary cause of the misses)

**Observation.** All 615 refusals sit inside the four burst windows; outside them both refusal counters are flat. The windows recur every ~40 s and last ~9 s. `src/host_dx12/j2/live_dlaa.cpp` already knows this cadence from the DLAA work: "SE5's menu bursts (every 40 s) hand the scene to a not-yet-learned entry one list earlier" (comment at `record_between_consumer`). The learned dispatch itself was seen on every frame in these windows (matched 300/300), so the step is there; what is missing is a usable scene producer.

**Mechanism.** The inline route (the one in use all run: `late{inline=17468}`) records DLSS right after the dispatch and, when the dispatch's list does not render the scene (`ListRendersTo` false), takes camera, depth snapshot and ordering from `g.native_scene`, which `NativeScaleHook::Closing` publishes when a scene-rendering list is **closed**. `reconstruct` refuses with "No unused executable or submitted scene producer…" when `saved.color!=color || saved.depth!=depth || !native_origin || !native_recording || native_serial<=consumed_native_source || (Retired && !Ready)`. Scene selection was stable (no scene_pick changes in discovery.jsonl, `j2 on … depth=… color=…` constant), so the live branches are `native_serial<=consumed_native_source` (the only producer published so far is last frame's, already consumed) and `Retired && !Ready`. Both mean: this frame's scene list had not closed when the upscale dispatch was recorded. SE5 records on several threads (camera_cut events arrive from five recording threads: 45264, 41504, 48716, 14408, 46412), and in the burst windows the game evidently records/closes the lists in a different order or splits them differently (the dumps from earlier runs show two layouts: pso 556 ending list 76 with the HUD in list 78, or pso 556 inside list 78 before the HUD blends). The second refusal (119, deferred route, "producer changed before the upscale recording was submitted") is the same condition seen from the submission side: the producer staged at record time was superseded, and the newest one lacked a depth snapshot (`depth_unknown` = 44 % of closes have no known depth state, so no snapshot is taken).

**What is confirmed:** the counts, their confinement to the windows, the periodicity, the stable scene selection, the sub-conditions of the refusal. **What is inferred:** which sub-condition fires (consumed vs. retired-not-ready) and the exact recording order in bursts; the log does not split them.

**Fix I propose (after approval):** when the inline route refuses for a stale producer, stage the step for submission (`stage_late`, which takes the producer from `g.native_scene` at ExecuteCommandLists, by which time every scene list of the frame has closed) instead of giving the frame up; and split the refusal into its sub-conditions so the next log names the exact one. This restores the frames in layout A (step ends its list). If a burst frame is layout B (step inside the HUD list, no boundary), it still misses; the split refusal plus `late{staged/resolved}` will show how many.

## Contender 2 — a missed frame falls back to the ordinary DLAA route and restarts DLSS history (why a single miss looks like a flash, not a soft frame)

**Observation.** 683 `camera_cut why=restart_resume` events; per bucket they track the misses (3300: 40 misses / 44 restarts; 3600: 65/77; 7200: 69/44; 11100: 74/99; 17700: 75/85) and the ngx_failed cluster (12900 bucket: 0 misses, 25 ngx failures, 36 restarts). `history_resets=0`, so the J2 router did not change route. `j2_coverage covered=16988` against 16768 Quality deliveries: 220 evaluations ran through the ordinary J2 DLAA route in this run, and `j2_reasons` sampled "Live DLAA is running inside the game's scene list" 293 times.

**Mechanism.** `restart_resume` is emitted when `tracker_restart = g.resume_reset || frame gap` with `g.resume_reset` true (`live_dlaa.cpp` ~3729, 3763). `g.resume_reset` is set true in four places; the one that fits a per-miss pattern is the working-texture reallocation in `record()` (~3678: "a new extent / format restarts DLSS history … remains pending if this recording is not submitted"). On a frame where the Quality route refuses, the ordinary inline DLAA route still has its scene boundary and runs; it evaluates the FP16 scene colour at render size, a different input format and size than the Quality route's RGBA8 render-size → 3440×1440 evaluation, so the working textures are recreated and `resume_reset` is set; the next Quality frame then restarts history. Visually each miss therefore produces two events: one frame of the game's own scaler (the miss) and one freshly-reset DLSS frame (aliased until history rebuilds). That is the "rapid" part of the flicker.

**What is confirmed:** the counts and their per-bucket correlation with misses and NGX failures; `history_resets=0`; the 220 non-Quality evaluations. **What is inferred:** that the reallocation path (not the extent path at 3641) is the specific `resume_reset` setter; both are in `record()` and both reset on an input change.

**Fix I propose (after approval):** while the native scale is reduced and the Quality step is learned, a frame that cannot deliver Quality must not run the ordinary DLAA route (it changes the picture and the DLSS inputs); and a one-frame gap must not reset history (the gap rule already tolerates `max_gap`; only the reallocation forces it). Then a residual miss costs one soft frame, not a flash.

## Contender 3 — the "later_written" phase: DLSS written, then the canvas written again in the same recording

**Observation.** From frame ~13500 to ~17100 every delivery has `later_written=1` (3335 of 16768). In that phase misses and restarts are near zero and Quality was delivered every frame, so this is not a cause of the burst flicker; it matters because if those later writes replaced the picture, Quality would have been invisible for 36 s while the counters said "delivered".

**Mechanism.** `later_written` counts `ListInsertionDecision(list, dest).writes` increasing after our inline DLSS in the same recording (`Execute`, ~7666). It cannot tell a preserving write (HUD blend onto the canvas) from an overwrite. The only dump of a layout-B frame we have (p27252 `frame-6422.txt`) shows, after pso 556 inside list 78, only pso 721/725 full-size blend draws with icon-sized inputs and then the screen draw pso 785 reading the canvas: HUD composition, preserving. The 13500–17100 phase is therefore most likely layout B with the HUD counted as later writes, which is the intended order (DLSS, then HUD on top, then screen). The delivery probe's `confirmed_in_presented` (47 sessions) supports that DLSS reached the screen in at least some frames, but the probe is not tied to this phase.

**What is confirmed:** the phase boundaries and that the deliveries and streak continued through it; the layout-B dump from a previous run. **What is not confirmed:** that no non-preserving write follows DLSS in this run's layout B; p36948 has only the frame-32 dump (layout A, step last in its list, 0 later writes).

**Fix / check I propose (after approval):** make `later_written` count only non-preserving writes (full, non-blend writes to the canvas), which the trace already distinguishes, and write one frame dump automatically the first time a non-preserving later write is seen. No visual change unless such a write exists; if it does, the DLSS insertion moves after it.

## Not contenders (checked and excluded)

- Learned route loss: `learned=556` on every trace after frame 647; the two `variant_change` lines at 587/617 are the initial learning. The scene id did not change.
- Depth-snapshot slots: `no_slot=0` all run (the 24-slot fix holds).
- Table late-binding: `late=N/N` on every trace; the submission-time resolution is working; the inline route chose its input from the recording's writes on all 17,468 frames.
- Alternating inputs: accepted (no "not the learned ones" refusals).
- NGX: 85 failures in 17,468, clustered at 9900–10500 and 13200; a minor contributor, visible through contender 2.

## Order of work if approved

1. Contender 1 fix + refusal split (removes most misses, names the rest).
2. Contender 2 fix (a residual miss no longer flashes).
3. Contender 3 counter change + automatic dump (confirms layout B is benign or finds the overwrite).

One run after all three, Quality mode, with the usual Results export; the log then shows: drops by named sub-reason, restarts, non-preserving later writes, and a frame dump of the first such write if any.
