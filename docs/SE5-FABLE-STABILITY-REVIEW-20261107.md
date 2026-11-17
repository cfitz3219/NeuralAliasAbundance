# Astra review of Fable's final Sniper flicker proposal

2026-11-07. Review of `claude/se5-quality-latebind` at `9c8bbf1` (proposal), runtime `abb6e12`. Reviewer: Astra. Requested by Cort; no new tests, captures, game actions, runtime edits or installer changes.

## Verdict

**Changes required; do not approve the proposal as a complete stability fix.** Approve the direction of resolving producers at submission, with the ordering corrections below. Preserve the discovered upscale boundary and native HUD. Replace the ordinary-DLAA-fallback explanation with the actual depth-representation reset defect. Correct the failure accounting and include the existing colour-state/camera failures. Do not redefine later writes so that potentially destructive work disappears from the evidence.

This is useful progress: the run records 16,768 successful native deliveries from 17,468 matched operations, a longest streak of 2,968, and pixel-probe confirmations that DLSS reaches presentation. Those figures establish a working insertion point. They also leave 700 unmatched-to-delivered operations, so they do not establish uninterrupted reconstruction. Counters here refer to operations/receipts, not an independently measured number of distinct displayed frames.

**Final question: “Will this be totally fixed and become a professionally stable product?” Answer for the proposal as written: no.** This is based on concrete uncovered paths and contradicted explanations, not a demand for more user testing. The corrected engineering work below is actionable from existing evidence. No additional capture request or review cycle is introduced.

## Evidence and reproducibility

- Proposal: `C:/Users/Cort/NA-wt-latebind/docs/SE5-QUALITY-FLICKER-AUDIT-20261107.md`.
- Exact runtime source: `C:/Users/Cort/NA-wt-latebind/src/host_dx12/j2/live_dlaa.cpp` at `abb6e12`. All source line references below use that snapshot.
- Run receipt: `C:/Users/Cort/Documents/NA-se5-results-20261107-063832/manifest.json`; its source commit is `abb6e1223f3645ef6bcc4e13e35c79a85c460ff4`.
- Log: the same directory's `status-36948.log` and `discovery.jsonl`.
- Images/metadata: `C:/Users/Cort/AppData/Local/NeuralAlias/captures/j2-20261107-063534-sniper5_dx12-p36948` and the five later p36948 sessions (`063537`, `063612`, `063815`, `063817`, `063819`). Earlier HUD evidence is documented in `docs/SE5-HUD-PIXEL-VERIFICATION-20261107.md`, commit `8197570`.
- Existing-data extraction: `C:/Users/Cort/NeuralAliasAbundance/local/se5-fable-final-audit-20261107/read_evidence.py` and `status-blocks.json`. This parses saved files; it does not execute the renderer or a test.
- Fable's previously reported 39/39 automated checks remain his evidence. This review did not rerun them or treat them as proof of uninterrupted game rendering.

## 1. Producer acquisition: correct target, incomplete proposed repair

The final event at log line 5149 reports 496 “No unused executable or submitted scene producer” refusals and 119 “selection or producer changed” refusals. These are real and clustered. Resolving the matching producer later is a good direction. However, neither message distinguishes all its subconditions, so “the current scene list had not closed” is an inference, not a proven explanation for every occurrence. The aggregate `depth_unknown` close count cannot prove why the 119 particular consumers failed.

`Closing()` publishes every eligible recording into the single `g.native_scene` (lines 5303–5338). `record_deferred_native()` takes that singleton or substitutes the newest one, then still requires its serial to equal the current singleton (5354–5403). CPU Close order does not establish GPU execution order. An unrelated or later scene recording can supersede the correct earlier producer. At submission, the lists in that submitted batch are closed; that does not mean every scene list for the frame is closed or belongs before this consumer.

**Concrete correction:** retain the small existing `SceneInput`/recording-lease records until consumed or retired, and choose the compatible producer by actual queue/submission position before this consumer. Keep colour/depth/subresource, recording generation, scene epoch and camera/jitter together. Do not discard a valid producer just because another list closed later. Reuse the existing bounded snapshot pool and submission bookkeeping; no global scan, GPU wait or new replay framework is needed for this producer-selection repair.

`stage_late` is approved for an actual tail boundary. The proposal explicitly leaves the known layout with upscale followed by HUD inside the same list unresolved. Source lines 5361–5362 reject that layout. Existing p27252 frame 6422 proves that it occurs. Preserve inline placement for those recordings using their local inputs or an already identified producer; do not move reconstruction after the HUD. A generic fallback after a failed `reconstruct()` also must distinguish a refusal before commands were appended from a partially recorded failure (`touched`), and must arm the tail watch after the original dispatch. The current `stage_late` API is designed to be called before the operation, with its `After()` callback arming that watch.

For an inside-list consumer whose necessary producer genuinely is unavailable at recording, the written proposal has no complete solution. The corrected producer records may recover cases currently lost merely because the singleton was replaced; they do not justify claiming every such case is solved. Keep that limitation explicit instead of calling the tail-only fallback final.

## 2. The claimed ordinary-DLAA fallback is contradicted by the run

Fable subtracts native delivery 16,768 from coverage 16,988. These are different snapshots. The last regular status block (06:38:23, log lines 5082 onward) has J2 frames 16,988 and native deliveries 16,695: difference 293. The 16,768 native count comes from the later frame-18600 event at line 5149. The sampled counters are not an atomic snapshot, so one-frame discrepancies should not be overinterpreted.

More decisively, the difference stays approximately zero during the early reset bursts:

| Status time | All J2 frames | Native deliveries | Difference | History restarts | Reduced mode |
|---|---:|---:|---:|---:|---:|
| 06:35:28 | 2,305 | 2,304 | 1 | 2 | 1 |
| 06:35:38 | 3,192 | 3,192 | 0 | 140 | 1 |
| 06:36:08 | 5,879 | 5,878 | 1 | 140 | 1 |
| 06:36:18 | 6,797 | 6,796 | 1 | 288 | 1 |
| 06:37:23 | 12,467 | 12,466 | 1 | 507 | 1 |
| 06:37:28 | 12,707 | 12,688 | 19 | 508 | 0 |
| 06:37:33 | 12,979 | 12,688 | 291 | 508 | 0 |
| 06:37:38 | 13,238 | 12,945 | 293 | 509 | 1 |
| 06:38:23 | 16,988 | 16,695 | 293 | 683 | 1 |

The log explicitly records panel setting `j2_scale_mode=0 was=1` at line 3825 and `j2_scale_mode=3 was=0` at 3968. The full-resolution interval explains the non-native evaluations. Discovery also changes scene identities at lines 6996 and 7277; captures change from 2293x960 to 1720x720. “Quality was on the whole time” and “scene selection never changed” are not correct for the entire run, although the early bursts occur within a stable scene selection.

Source already excludes ordinary inline DLAA while `g_native_reduced` is true (4412–4418) and excludes the between-list fallback in reduced mode (7679–7682). Short gaps are already tolerated: four frames, or 32 within the same selection epoch (3726–3732).

**Alternative:** preserve these existing rules. Do not suppress legitimate DLAA selected at full resolution, remove meaningful resets, or implement a second redundant miss-frame gate. Fix the representation change below.

## 3. Actual unnecessary-reset defect: original depth versus its R32 snapshot

The code supplies original game depth for a local inline producer and `native_depth_snapshot` for a captured producer (3390 and 5173–5217). The snapshot is always R32_FLOAT (4714), while p36948's original selected depth #13417 is format 19 with view 21, visible in its frame-32 dump. Both feed the existing single-sample plane-zero conversion to internal R32 depth.

Nevertheless `record()` includes `g.depth_format != dd.Format` in the condition that rebuilds *all* working textures (3643–3646), saves the raw source format and sets `g.resume_reset=true` (3677–3678). Switching between equivalent original depth and its copied R32 representation therefore forces a history reset despite unchanged scene/output dimensions and unchanged temporal content.

The saved evidence shows this path change during the first burst. At log line 618 the depth-source counters have 2,305 inherited original-depth evaluations and zero snapshot/supplied evaluations. By line 1196 they have 2,647 inherited, 27 local and 518 in source category 4 (`depth_submitted`, used for supplied snapshot state here). History restarts rise from 2 to 140 while the all-J2/native difference stays zero. This establishes the depth-path transition and the source-level reset mechanism. It does **not** establish an exact one-to-one attribution of all 683 events; the log does not record the allocation key at every reset.

**Concrete repair approved:** separate raw source-depth copy/resolve configuration from the canonical temporal working-image key. For compatible single-sample 32-bit plane-zero sources, original depth and its R32 snapshot must share working storage/history. Continue validating each source's actual format, subresource and state for its copy. Keep real dimension, sample-count, MSAA-resolve, scene-change, user-toggle and long-gap resets. Do not simply clear `resume_reset` after a miss. This reuses the existing conversion and removes both needless allocations and needless resets across games.

## 4. The 85 “NGX failures” are acquisition failures, including 72 colour-state misses

`q_failed` increments whenever `record()` returns false (5280–5284), but is printed as `ngx_failed`. Actual failed `na::dlss::Evaluate` calls increment `g.stats.dlss_failed` at 4162–4163.

All 44 regular status blocks report actual `dlss_failed=0`. From the first active block to the last, `no_state` rises from 1 to 73 and `no_camera` from 4 to 17: **72 + 13 = 85**. The log explicitly identifies the state failures as **colour 73, depth 0**, not depth-state failures. At the final matched-operation event: **17,468 - 16,768 = 700 = 496 + 119 + 85**. The separate gate-drop count of 618 is not an interchangeable denominator for these 700 operations.

**Correction:** call this counter `record_failed` and preserve its reason categories. Repair colour-state acquisition from the actual input recording/submission prefix and camera acquisition from the matched scene producer. The inline `require_current_states` path currently accepts only the consumer's own known state or a state already established on the queue (3420–3437); a closed predecessor in the pending batch cannot supply that state through this path. Submission-time resolution can use the existing `state_after`/entry-state machinery when a legal splice exists. Preserve exact current barriers; do not invent a default state. Keep the current camera/jitter evidence paired with its producer rather than selecting the newest camera independently. These are actual missed submissions and remain part of completion, not a minor NVIDIA issue.

## 5. A late order check currently cannot cancel inline GPU work

The proposal builds on 70fc7e5's permission to record inline against an unsubmitted producer. `Execute` computes `producer_order` (7659 onward), but then forwards the original lists unconditionally (7710). A false result changes the delivery accounting and resets history afterward; it does not prevent the already recorded DLSS commands from reading unwritten inputs. The comments describing this as refusing delivery are stronger than the behavior.

This run records `order_refused=0`, so this is **not asserted as its observed flicker cause**. It matters to the requested professional stability and transfer across games.

**Alternative:** establish the inline dependency before appending dependent commands, using local earlier work or an already qualified producer. When the dependency can only be established at submission and a tail boundary exists, use the owned-list splice. Do not reorder game work or pretend that rejecting its receipt cancels it. Preserve per-recording resource leases through submission/completion. This fixes an execution contract, not a game-specific identifier.

## 6. Later writes: preserve the useful evidence and the native HUD

Fable is right that the 3,335 later-written deliveries form a different regime and do not track the main burst misses. Earlier saved layout-B drawings and the HUD pixel comparisons support preserving the later native UI.

However, the trace's `preserves` label is not a proof: it includes `blend || depth`, and elsewhere partial viewport/scissor conditions (572, 696). A blend can replace its destination, a depth-enabled draw can overwrite the scene, and a partial write can damage part of it. Counting only full, non-blended writes would hide these cases. Existing `blend_class` in `observe/producers.cpp:2101` supplies a more useful distinction than a boolean blend flag, but even ordinary alpha blending can become an overwrite where shader alpha equals one.

**Alternative:** retain the raw later-write count; optionally classify known UI composition, possible replacement and unknown using the existing operation/coverage/blend metadata. Do not infer final-pixel preservation from the classification alone. Do not automatically move DLSS after an arbitrary later write: that could overwrite the native HUD already verified in saved images. If another write supplies the scene that reaches the screen, select the actual scene-to-screen boundary using the existing dependency trace, before its subsequent UI. The proposed automatic extra capture is not required or approved by this audit; Cort asked to use existing evidence.

## Focused implementation disposition

1. **Approve with corrections:** producer resolution using actual execution order and retained recording identity; reuse tail splicing only where that boundary exists, preserve the current inline/HUD placement elsewhere.
2. **Approve:** normalize the working-depth representation key so equivalent original/snapshot depth does not rebuild temporal resources.
3. **Include:** resolve the 72 colour-state and 13 camera misses through those same producer/ordering mechanisms; correct the misleading counter label.
4. **Required correctness correction:** an after-the-fact order refusal must not be advertised as preventing recorded GPU work.
5. **Reject as written:** the phantom ordinary-DLAA fallback cure and the non-blend-only overwrite counter. Use the alternatives above.

All of these target existing code and actual failure paths. No new generic framework, game-specific PSO whitelist, periodic blackout/warmup workaround, full-frame replay project, new user diagnostic run or unrelated image-quality feature is requested. Fable retains implementation ownership; Astra has made no competing runtime edit. This approves useful corrected engineering directions, **not final product completion**.
