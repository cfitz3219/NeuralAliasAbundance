# DLAA placement: a proof-based design (response to Cort's advisory brief, 2026-10-27)

**Author:** Opus (`claude/p4-autoroute`, head `f74223e`). Source was inspected for every claim marked *(source)*; everything else is marked as a hypothesis or an open question.
**Scope:** DLAA placement and delivery to the displayed scene. Not Frame Generation, not a new capture framework, not input reconstruction.

---

## 0. The one design, in a paragraph

Stop predicting a single "the" consumer and stop locking a single route for the whole game. Instead:

1. **Recognise operations by role class, not identity.** A consumer's class is *(operation kind, root-signature identity, destination size/format class, scene-colour SRV count)*: shader variants of the same pass share a class; mid-scene readers (water, refraction) and the post chain usually do not.
2. **Learn a small profile per game: a set of accepted post-entry classes with per-class hit/miss confidence, and a set of rejected temporary-reader classes.** Learned retrospectively from *submitted* recordings, where the final version of the scene colour is a fact. This replaces today's one-signature anchor, which treated SER's alternation between two legitimate structures as "instability".
3. **Decide each frame from facts that are already known at that moment, and predict only the one thing that cannot be known: which read in an open recording starts the post chain.** At submission time the recording is complete, so "does the scene list contain a consumer of its final version?" is a fact. Only the inline trigger predicts, and it predicts a *class*, backed by the profile.
4. **Arm both insertion mechanisms every frame; let the facts pick.** Inline fires when a recognised post-entry class appears after a scene write. If the scene list is submitted without any post-write consumer, between-list insertion follows it (existing code). If it is submitted *with* an unrecognised consumer, that frame is an honest miss that teaches the profile. Nothing is ever disabled on weak evidence.
5. **Establish contribution separately, from lineage where reads are observable, and from the capture pairs we already have otherwise.** Placement correctness and screen contribution are different conclusions and get different evidence.

What this improves over the current build: the anchor no longer breaks on SER-like alternation (same-list on some frames, later-list on others) or on shader variants; the 16-frame gate and its fallback become unnecessary; the between-list route stops being a global choice and becomes the per-frame completion of an inline miss-by-absence.

---

## 1. Recognising roles from inputs, outputs, shader characteristics and neighbours

### 1.1 What Luma does, and what transfers

Luma's generic Unreal path recognises UE4's TAA pixel shader by **shader structure**, not by hash: it walks the DXBC declarations for the largest constant buffer, counts declared textures in a token range, expects the 9-tap neighbourhood loop, and finds the four `PrevClipToClip` matrix indices by swizzle pattern; it then reads the per-view global constant buffer for the projection matrix and gates on the pass's own render-target size ([PR #206](https://github.com/Filoppi/Luma-Framework/pull/206)). Its game folders remain hand-marked shader replacements ([Luma README](https://gh.r-mj.com/Filoppi/Luma-Framework)).

- **Engine-specific (do not port):** the UE4 TAA shader layout, the global CB layout, the DX11 shader-replacement model.
- **Transferable:** classify a pass by *what it is shaped like* (bound-resource characteristics, constant-buffer shape, declared texture count) and *where it sits* (its render target vs the view size), with the per-view CB as the camera source. We already have the camera part (UEVR/Geo-11 VP + inverse pairs, Argon's drop-in). The shape part is what Section 1.2 adds.

### 1.2 Role class for a scene-colour consumer

Every observed read of the selected colour already yields *(source)* an `InsertionEvent{resource, destination, pipeline, write, op, root}`: op ∈ {GraphicsRead, ComputeRead, CopyRead, Barrier, …}, `root` = the root-signature object. At PSO creation we record *(source: `na_ev_pipeline_created`)* per-stage bytecode hashes and sizes, RTV formats, DSV format, sample count, depth and blend state. Bytecode itself is not retained and there is no DXIL reflection today.

Define the **class key** of a consumer event as:

| field | source | why it survives variants |
|---|---|---|
| op kind (graphics / compute / copy / resolve) | event | a pass does not change kind between variants |
| root signature id | event | permutations of one shader share the root signature in practically every engine; the mid-scene readers in SER's log used *different* roots (…1136 vs …2368) |
| destination class: (width, height) relative to the view, format | destination resource desc (already acquirable) | tonemap/bloom-entry targets keep their size/format class; the resource object rotates per frame |
| scene-colour SRV count in the table (0/1/2+) | `before_shader_use` already walks the table and knows how many SRVs point at the colour | distinguishes "reads scene once" from history-style passes |
| **not** the PSO id, **not** the destination object id | | these are the fields that varied in SER |

Optional refinement (not needed first): at PSO creation parse the DXIL container's `PSV0` part (resource bind table: kinds and counts, no reflection library needed) and add "declared Texture2D SRV count" and "largest CBV size" to the class. That is the Luma-shaped signal in DX12 form. It is an enhancement because the four fields above already separate the cases we have evidence for.

### 1.3 Three different conclusions

| conclusion | evidence that establishes it | status today |
|---|---|---|
| **A suitable scene image** (which colour texture, with which depth) | ReShade `generic_depth` ranking for depth; the colour drawn together with it; subresource identity *(source: `ListSceneSubresource`)* | working in both games |
| **A suitable insertion point** (after the last write of the final version, before its first consumer) | *retrospective*: complete recording + submission order; *live*: class recognition backed by the learned profile | what Sections 2–3 change |
| **Contribution to the displayed scene** | observed version lineage from our output to the presented buffer; else paired output/presented captures | `contribution=unknown` unless lineage observed; capture pairs exist *(source: `capture/j2_capture`)* but are not yet compared |

A correct insertion point does not prove contribution (an unobserved bindless read could consume an earlier copy), and a positive contribution does not prove the point was the *best* one (DLAA before a tonemap that then reads a different buffer still "contributes" nothing visible). Keep them apart in the log.

---

## 2. The decision process

### 2.1 Following the scene through its consumers

Within one recording the trace *(source: `InsertionTrace`)* is an ordered list of writes and reads of the selected colour. Define the **version** of the colour as "number of writes so far". A read of version *v* followed by a write creates version *v+1*: that read was temporary. The **final version** of a frame is the one with no later write *in submission order across all lists before Present*.

Live, in an open recording, a read is a *candidate*; whether it was the final version's consumer is only knowable later. That asymmetry is the whole problem, and the design treats it as such:

- **Retrospective (at submission, fact):** for each submitted recording that wrote the colour, `AnalyzeConsumer` *(source)* finds the first actual consumer after the recording's last write, and the aggregator *(source: `FrameAggregator`)* knows whether a later list wrote the colour again this frame. From that, each consumer event is labelled **post-entry** (first consumer after the frame-final write), **temporary** (a later write followed), or **unknown** (recording truncated at 8192 events *(source)*, or bindless/indirect reads suspected because the colour was transitioned to a read state and no read was observed).
- **Live (prediction):** the inline trigger runs at the actual consuming draw/dispatch/copy *(source: `before_shader_use` / `before_texture_copy`)*, computes the class key of that consumer, and fires only if the class is in the profile's **accepted** set and not in its **rejected** set, with at least one scene write before it in this recording.

### 2.2 Temporary read vs. start of the post chain

Evidence that distinguishes them, in order of strength:

1. **A later write to the colour in the same frame** (retrospective, decisive): temporary.
2. **Class membership** (live, learned): a class seen as temporary in ≥ N frames and never as post-entry is rejected; a class seen as post-entry and never temporary is accepted; a class seen as both is ambiguous and requires context (producer PSO of the preceding write, as the existing `ProducerConsumer` mode does) or is left to the retrospective between-list path.
3. **Destination relation**: a consumer whose destination is written and then *read back into the scene colour* (refraction/particle composite) is temporary by construction; a consumer whose destination never feeds the colour again is post-chain. Available from the same trace.
4. **Depth binding is *not* used as the criterion.** Transparent, sky and particle passes write the colour with depth testing off or no DSV; the last depth-bound draw does not complete the scene. The trace counts any draw with the colour bound as a render target as a write *(source: `note_draw` adds `cur_rts`)*, which is the right definition.

### 2.3 Inline vs. between-list, per frame

The properties *graphics/compute*, *same list/later list* and *inside a render pass* are orthogonal flags on each consumer event, not engine types. The per-frame procedure:

```
on Execute(batch):                       # submission order, lists complete
  for list in batch, in order:
    if list rendered the scene (colour+depth):
      sig = AnalyzeConsumer(list)         # fact for this recording
      if inline fired in this recording:  # our DLAA is inside it
        commit inline (existing path)
      elif sig has a post-write consumer: # we missed inside this list
        count MISS; learn its class as post-entry if frame-final (decided at Present)
        do not insert between lists (too late: the read already happened)
      else:                               # no consumer in this list
        if structure says this is the frame's last scene list (existing batch histogram):
          insert between lists right after it (existing path, within the batch)
        else: wait for the later scene list
on Present:
  close the frame: label consumers post-entry / temporary using "any later write?"
  update profile: hits, misses, class promotions / demotions
```

Nothing here changes the mechanism of either insertion path; it changes who decides. The router *(source: `route_selector.hpp`)* keeps its roles of reporting, interceptor ownership and contradiction counting, but it no longer gates Execute on a 3-frame route lock: the lock is replaced by the per-list fact above.

### 2.4 Cold start and adaptation

- **Frame 1..k:** no profile. Inline never fires (no accepted class). The scene list is submitted; if it contains a consumer, that frame is a miss and teaches; if it does not, between-list inserts after it (as today). This is the existing behaviour and it is correct: a cold frame can only be placed where the facts already allow.
- **Structure change (new post shader, scope mode, new list split):** the new class is unrecognised → one missed frame per new class → learned at that Present → recognised from the next recording that starts after the Present. Because learning is per class, a game that alternates between two structures keeps *both* accepted; nothing oscillates.
- **Contradiction:** a class accepted as post-entry that is later seen followed by a write is demoted (temporary), and the frame that executed on it is counted as a placement contradiction (existing counter). Demotion needs one observed later write, not a vote, because a later write is a fact.

---

## 3. Evaluation of the proposed additions

| addition | verdict | reasoning |
|---|---|---|
| **HUD heuristic** (blend on, depth off, no DSV → HUD) as a vote/veto | **implement as a logged corroboration only; no veto now** | Additive bloom and lens-flare composites are blend-on/depth-off/no-DSV draws onto the post target; they would read as `hud_begin` and a veto would disable working DLAA (the failure mode the brief warns about). Useful as a *consistency statistic*: the fraction of frames where our insertion precedes `hud_begin` of the same recording. Promote to a veto only if that statistic is 100% in both games over long runs. |
| **Injected image marker** | **defer; use the existing capture pairs first** | We already capture DLAA output and the presented image per slot. Offline correlation of those pairs establishes contribution for a run without touching the live image and without a marker's ambiguity (absence can be filtering). A marker only becomes worth its cost if we need *per-frame live* contribution, which nothing in the current plan needs. Lineage tracking (copies/resolves from our output's destination to the back buffer) is the live, observation-based form and should come first. |
| **16 identical signatures** | **retire once the class profile exists; keep the fall-back behaviour** | Frame changes are not independent: SER alternates. A run-length gate on a single signature is the wrong model for that; a per-class confidence (hits/misses, with a class accepted after a small fixed number of post-entry observations, e.g. 3, like the route lock) is the right one. Keep the principle it encoded: a new predictor never *disables* the proven path; a miss teaches. |

Implement now: the class key, the set profile with confidence, the per-list fact-based decision in Execute, the HUD consistency statistic. Defer: PSV0 parsing, lineage tracking (next), marker (only if lineage proves impossible in a real game).

---

## 4. DX12 invariants (what the design relies on)

- **Submission order vs. recording order:** learning and frame-final labelling happen in Execute/Present (submission order) *(source)*; a closed-but-unsubmitted recording teaches nothing and cannot replace the profile; recording generation keys every per-list fact *(source: `ListRecording`)*.
- **Resource and subresource identity:** the class key uses *destination class*, never object ids; the scene copy uses the exact subresource the scene was drawn into *(source: `csub/dsub`)*.
- **Queue dependencies:** J2 follows one queue *(source: `ensure_device`)*; the between-list insertion stays inside the same batch on that queue *(source: `batch.insert(scene+1)`)*; the backend's fence dependency for the previous evaluation is unchanged *(source: `dlss::BeforeExecute`)*.
- **Resource states:** inline uses the state recorded so far in the list, else the known submitted state *(source: `ListLastStateSub` / `KnownSubStateNow`)*, and restores exactly the transitions it adds.
- **Render passes:** no insertion while `in_render_pass` *(source: `ListStateRestorable`)*; the first legal point is after `EndRenderPass`, where bindings are undefined and the game rebinds before its next draw.
- **State restoration:** `RestoreListState` re-applies heaps, root signatures, root arguments and PSO *(source)*. It does **not** restore viewport, scissor, topology, render targets, blend factor or stencil ref. Our inline work is copies, barriers, compute dispatches and NGX (compute), which leave those untouched. The one exception is the MSAA depth-resolve draw, which is why MSAA-depth graphics anchors stay on the boundary matcher until that draw's raster state is shadowed too (optional enhancement: shadow `RSSetViewports/ScissorRects`, topology and the current RT set, all already observed, and re-apply them).
- **Jitter:** unchanged. Inline applies this frame's sample at the trigger before reading the camera, and the record-first path patches an earlier pre-pass *(source: `jitter_batch(one,1)`, `inline_camera_submitted`)*. The only requirement the class design adds is that the trigger still runs *before* the consuming draw, which it does.
- **Bounded callback work:** the live trigger does one backward walk of the recording's trace to the last write (`Live`) and one table walk (`before_shader_use`), both O(events in this recording), with the trace capped at 8192 events *(source)*. No GPU readback, no waits, no allocation on the hot path beyond the existing ring.

---

## 5. Implementation handoff

### 5.1 Components to extend (all existing)

- `src/host_dx12/j2/insertion_trace.hpp`: add `ClassKey` to `InsertionEvent` consumers; `AnalyzeConsumer` returns the post-entry class and the list of temporary-reader classes of the recording.
- `src/host_dx12/observe/producers.cpp`: in `before_shader_use`, fill the key's SRV count and destination class (the destination resource desc is reachable through `g_objs`); copies/resolves fill op kind and destination class.
- `src/host_dx12/j2/live_dlaa.cpp`: replace the single `insertion_learned.consumer` with a `ConsumerProfile` (bounded map class → {post_hits, temp_hits, last_seen_frame}); the trigger consults it; Execute labels per list; Present promotes/demotes.
- `src/host_dx12/j2/route_selector.hpp`: keep as the reporter; remove the Execute gate on `route == Between`; feed it the per-frame facts it already takes.
- Status log: `j2_profile accepted=N rejected=M ambiguous=K hits=… misses=… last_miss_class=…`; keep `j2_last_miss` with the class fields.

### 5.2 Additional data (only what is needed)

```cpp
struct ClassKey {            // identity of a pass role, stable across variants
    uint8_t  op;             // GraphicsRead / ComputeRead / CopyRead / ResolveRead
    uint64_t root;           // root signature object (0 for copies)
    uint32_t dst_w_class, dst_h_class; // destination size relative to view: 0 exact, 1 half, 2 quarter, 3 other
    uint32_t dst_format;     // DXGI_FORMAT of the destination (0 for buffers)
    uint8_t  color_srvs;     // scene-colour SRVs in the bound tables (0,1,2+)
};
struct ClassStats { uint32_t post_hits, temp_hits, misses; uint64_t last_frame; };
struct ConsumerProfile {     // per selected colour+depth pair, bounded (e.g. 32 classes, LRU)
    std::vector<std::pair<ClassKey, ClassStats>> classes;
    bool accepted(const ClassKey&) const;   // post_hits >= 3 && temp_hits == 0 (or producer-qualified)
    bool rejected(const ClassKey&) const;   // temp_hits > 0 && post_hits == 0
};
```

### 5.3 Pseudocode

```
# live, inside the game's recording, at the consuming draw/dispatch/copy
on_consumer(list, event):
    if !inline_allowed(list): return                      # render pass, latched, no device
    if no scene write before event in this recording: return
    k = class_key(event)
    if profile.rejected(k): skip (count temp_skip); return
    if !profile.accepted(k): skip (count unknown_skip, remember k for the miss record); return
    if this recording already has our DLAA: return
    record DLAA inline before the event (existing record(); jitter as today)

# at ExecuteCommandLists, per list, in order
on_submit(list):
    if list did not render the scene: continue
    sig = AnalyzeConsumer(list)                           # fact
    if inline recorded in list: commit (existing)
    elif sig.has_post_write_consumer:
        miss += 1; pending_learn.push(list consumer classes)     # labelled at Present
    elif list is the frame's last scene list (existing histogram):
        insert our list after it in this batch (existing)
    else: wait

# at Present
on_present():
    final_version_written_later = (any scene write after the last consumer we saw)
    for each consumer class c seen this frame, in recording order:
        if a write followed c this frame: profile[c].temp_hits++
        elif c was the first consumer after the frame-final write: profile[c].post_hits++
    for each class that was accepted and executed this frame but a write followed: contradiction++
    publish profile stats; route reporter gets executed/consistent/contradiction as today
```

### 5.4 Two worked traces

**Anno-like (between-list).** Frame: list A (depth pre-pass), list B (scene writes, no reads), list C (post: reads colour into tonemap target), Present.
- Live: B contains no consumer → trigger never fires.
- Submit {B}: `AnalyzeConsumer(B)` = none; histogram says B is the last scene list → insert our list after B in the batch. (If the game submits {B, C} in one batch, insertion goes between B and C *(source)*.)
- Present: C's consumer class is first-after-final-write → `post_hits++` for that class. It never matters for Anno's placement (it is in a later list) but it is recorded, so if Anno ever moved the tonemap into B, the inline trigger would already recognise it.

**Sniper-like (variants and alternation).** Frame type α: list S = [scene writes, barrier, water read (root R1, PSO p1), scene writes, barrier, tonemap read (root R2, PSO p2 or p3), bloom …]. Frame type β: S has no water pass; type γ: S ends after the scene writes and the tonemap read is in a later list T.
- Cold frames: α → water class (R1, exact-size, fmt F1, 1 SRV) gets `temp_hits=1`; tonemap class (R2, exact-size, F2, 1 SRV) gets `post_hits=1`; the frame is a miss. After 3 α/β frames, the tonemap class is accepted.
- α live: water read → class (R1,…) rejected → skip; tonemap read with p3 instead of p2 → same class (R2,…) → fire. **Variant handled.**
- β live: no water read; tonemap read → fire. **Changing intermediate reads handled** (no ordinal involved).
- γ: S is submitted with no consumer → between-list insertion after S (if the histogram marks it last; the following list T reads it). **Alternation handled**, with no profile change and no "instability".
- A genuinely new post shader with a *new root signature* (scope mode): one missed frame, learned, recognised from the next recording.

### 5.5 Steps

Essential:
1. `ClassKey` + profile with hit/miss confidence; `AnalyzeConsumer` returns per-class labels. CPU test: the Sniper-like α/β/γ sequences above, plus the shared-root ambiguity case.
2. Trigger matches on class; Execute decides per list from facts; Present labels. Host tests: existing inline/auto cases plus one alternation case (α, γ, α, γ …) asserting ≥ (N−cold) covered with 0 later writes.
3. Status-log lines; retire the 16-frame gate (keep "a miss never disables").
4. HUD consistency statistic (log only).

Optional, in order of value:
5. Lineage tracking from our output's destination through copies/resolves to the presented buffer → `contribution=established` live.
6. Offline capture-pair comparison tool for runs (contribution evidence without a marker).
7. PSV0 parse at PSO creation (declared SRV count, largest CBV) into the class key.
8. Raster-state shadowing (viewport, scissor, topology, RT set) so MSAA-depth graphics consumers can use inline.

### 5.6 What is established, what is not

Established from observations: scene image selection; the final version of the colour per frame; whether a submitted scene list contains a consumer; the class of every observed consumer; whether our insertion was followed by a scene write.

Hypotheses until measured in SER: that SER's variants share a root signature per role; that its alternation is between exactly the α/β/γ shapes; that no bindless read of the colour exists before the first observed consumer.

Not knowable from the API alone: reads through bindless/indirect paths that bind no descriptor table we can see (the barrier to a read state is still observed, so the design treats "transitioned to read, no read observed" as *unknown* and does not insert on it); UAV-produced scene colour (no RTV write; would need the UAV write path classed as a write); multiple simultaneous views; cross-queue consumption. Each of these is reported as a named gap rather than guessed.

Sources: [Luma UE TAA detection, PR #206](https://github.com/Filoppi/Luma-Framework/pull/206) · [Luma README](https://gh.r-mj.com/Filoppi/Luma-Framework) · project source at `f74223e` (`insertion_trace.hpp`, `producers.cpp`, `live_dlaa.cpp`, `route_selector.hpp`, `capture/j2_capture.*`).

---

## Astra's response to Fable — 2026-10-27

Cort identifies the preceding proposal as Fable's response; its original author header is preserved unchanged. At Cort's request, the analysis below is appended for Fable to answer. No tests or game changes performed.

**Fable’s central idea is a useful improvement, and I would build on it. But the specification needs several corrections before implementation.** It replaces the single fragile shader match with a small collection of learned rendering patterns, while retaining both insertion methods. That directly addresses the kind of variation reported in Sniper.

I read the full response and checked its claims against our existing code. No tests or game changes performed.

**What Fable gets right**

- **Learn several valid patterns.** A game alternating between two legitimate rendering arrangements should not constantly forget one and relearn the other.
- **Group shader variants by their characteristics.** Different shaders can perform the same job. Exact shader identity should not be the only matching criterion.
- **Allow inline and between-list insertion to cooperate.** Choosing one route permanently for an entire game is unnecessarily restrictive.
- **Reuse the existing infrastructure.** This is an extension of our recorder, matcher and injector—not another framework.
- **Defer the invisible marker and avoid a HUD veto.** Those are sensible scope decisions.

Those are worthwhile changes. The following issues affect whether they actually solve our problem.

**1. It still assumes we can know the last scene list too early**

The proposed code says:

> “if structure says this is the frame’s last scene list (existing batch histogram)”

A histogram of the current submission cannot tell us whether the game will submit another scene-writing list afterward. It can identify the last relevant list **in that batch**, not necessarily the whole frame.

This matters because inserting too early could leave later scene content untreated or overwritten.

**Correction:** distinguish observed facts from learned expectations. Where possible, place between-list work immediately before an observed downstream consumer, using the completed submissions available at that moment. Where it still relies on past structure, describe it as a prediction rather than proof.

**2. Its “role class” can still confuse unrelated passes**

Fable groups consumers using their root signature, output dimensions/format and number of references to scene color.

That is a useful starting fingerprint, but it is not yet recognition of what a shader does:

- Different operations can share the same root signature and output shape.
- The number of bindings pointing to current scene color does not reveal whether a separate history texture is also being read.
- Root-signature **object IDs** are session identities; they cannot serve as durable saved-profile keys.

**Correction:** retain this inexpensive grouping, but resolve collisions using the surrounding producer/consumer relationship and meaningful input characteristics. Use a structural signature for persisted root layouts. This can remain small; it does not require a general-purpose shader-understanding system.

**3. “A later write means this was a temporary reader” is too absolute**

Consider this sequence:

1. Copy the completed scene into another texture.
2. Continue post-processing from that copy.
3. Clear or reuse the original texture.

Fable’s rule could reject the copy because the original was written again—even though the displayed image follows the copied branch.

**Correction:** follow the relevant image through copies and downstream consumers. A later write to the original is evidence to examine, not automatically proof that the earlier consumer was wrong.

This means **a minimal amount of image-lineage tracking belongs in the essential work**, rather than being entirely deferred. We need the relevant scene chain, not a complete reconstruction of every resource in the game.

**4. Arming both routes needs an explicit shared execution decision**

The proposal checks whether inline work occurred in a particular recording. That alone is insufficient when another recording in the same frame might also receive between-list work.

**Correction:** both routes must share the existing frame/view execution ownership: one accepted DLAA evaluation, one corresponding jitter sample, and consistent history handling. Also specify what happens to recorded but unsubmitted inline work. This is integration with existing machinery, not a new subsystem.

**5. Some claimed “proof” is weaker than stated**

Two important examples:

- **Similar output and presented captures do not prove contribution.** Native and DLAA images depict the same scene and can correlate strongly even if the DLAA result never reaches the screen.
- **Three successful observations do not establish a permanent role.** The document also promises “one missed frame” for a new class while its acceptance rule requires three observations. That needs one consistent description.

Keep capture comparisons as supporting evidence. Use observed image relationships to support contribution, and describe the learned classifier honestly as adaptive recognition.

There is also a source correction: the cited Luma PR #206 was **closed**, with its author narrowing the supported claims. Its discussion explicitly questions some sizing and detection assumptions. It is useful reference material, not proof that the whole proposed technique is established upstream. [Luma PR discussion](https://github.com/Filoppi/Luma-Framework/pull/206)

**My recommendation**

Proceed with Fable’s direction, with this bounded implementation:

1. Learn multiple consumer patterns instead of one exact signature.
2. Track the relevant scene-image chain enough to distinguish continuation, copying and reuse.
3. Choose inline or between-list placement using that evidence.
4. Keep both routes under one frame/view evaluation and jitter decision.

**This is a credible improvement over the failed single-anchor approach. It is not yet a complete “proof-based” solution as written—but the corrections are focused, and they do not require starting over.**

---

## Opus's revision after Astra's response — 2026-10-27

All five corrections are accepted. They change the specification as follows; where a correction and the original text differ, this section governs.

**1. The frame's last scene list is a prediction, not a fact.** Agreed. Within one submission batch the existing code already does the right thing: it inserts after the last scene writer that precedes the first observed reader in that batch *(source: the `before_reader` scan in Execute)*, so a consumer in the same batch is placed before, not after. Across batches, "this is the last scene list" comes from the batch histogram and is a prediction. The spec now says so: between-list insertion is **placement before an observed consumer when one is in the batch**, else **placement after a predicted last scene list**, counted separately (`between_observed` / `between_predicted`). A later scene write after a predicted placement is a contradiction and demotes the prediction, as today.

**2. The class key is a fingerprint; collisions are resolved by context; persisted keys are structural.** Agreed.
- The key stays cheap (op kind, root, destination size/format class, scene-colour SRV count), but a class seen as both post-entry and temporary is **ambiguous** and is qualified by the preceding producer, which the existing `ProducerConsumer` mode already does. An ambiguous class without a distinguishing producer is not accepted.
- For a saved profile, the root-signature object id is replaced by a **structural hash of the root layout** (parameter types, ranges, register spaces), which producers already parse *(source: `root_params` / `root_ranges` in `g_objs`)*. Object ids remain the live key within a session.
- History-texture reads: the SRV count covers the scene colour only. Whether a separate history texture is read is not in the key. Adding "number of view-sized SRVs other than the scene colour" is cheap from the same table walk and goes into the essential key.

**3. A later write does not by itself make the earlier consumer temporary.** Agreed, and this is the most important correction. The rule becomes: a consumer is temporary only if, after the later write, **the original colour is observed being consumed again** this frame. If the later write is a clear or reuse with no subsequent observed read of the original before Present, the earlier consumer (typically a copy or resolve) was the real exit of the scene, and the displayed image follows its destination.
- This needs minimal lineage: the trace already records copy/resolve destinations of the colour as writes *(source: `after_texture_copy`)*. The essential addition is to extend the read check in `before_shader_use` from "the scene colour" to "the scene colour or a texture derived from it this frame" (bounded set, cleared at Present), so a read of the copy counts as continuation of the chain. That is the "relevant scene chain", not a reconstruction of every resource.
- Essential, not deferred: items 5 (lineage) and 1 (classes) in the step list merge into one first step.

**4. One execution owner per frame and view.** Agreed. The spec now states it explicitly, all from existing fields *(source)*:
- one DLAA evaluation per frame: `inl_frame == frame` or `submitted_frame == frame` blocks both the inline trigger and between-list insertion;
- one jitter sample per frame: the sample established at the first camera batch (`g.jit`), applied by whichever route records;
- a recorded but unsubmitted inline recording expires at Present: its slot is released, its capture token discarded (`Discard(token, true)`), the frame counted as a miss, and `inl.valid` cleared so the next frame can record. Today `inl.valid` persists until submitted, which the `pending` rejection counter already exposes; this makes the expiry explicit.

**5. Weaker proof, stated as such.**
- **Capture pairs are supporting evidence, not proof.** A presented image correlating with our output also correlates with the native scene. The usable comparison is three-way: presented vs our output vs our input (the pre-DLAA colour). If the presented image matches the output better than the input, after allowing for the game's tonemap, that supports contribution; it does not establish it. Contribution is established only by observed lineage (item 3 extended to the back buffer) — and the doc now says so in §1.3.
- **Acceptance rule made consistent:** a class is accepted after **3** post-entry observations with no temporary observation, so a genuinely new post shader costs up to 3 missed inline frames (fewer when the between-list path covers them). "One missed frame" is withdrawn.
- **Luma PR #206 was closed,** and its discussion questions some of the sizing and detection assumptions; it is reference material for the *kind* of signal (shader structure, bound-resource shape), not proof that the technique is established upstream. Noted in §1.1.

**Revised essential steps.**
1. Class key (with structural root hash and the extra view-sized-SRV count), per-class profile with 3-hit acceptance and producer qualification for ambiguous classes, **plus** minimal lineage: derived-texture set per frame and the "consumed again after the write" rule. CPU tests: the α/β/γ traces, the copy-then-clear branch, the shared-class collision.
2. Trigger matches on class; Execute decides per list (observed consumer before / predicted last list after, counted separately); Present labels, promotes, demotes and expires unsubmitted inline work. Host tests: existing cases, alternation α/γ, copy-branch continuation.
3. Status lines: `j2_profile …`, `between_observed/predicted`, `expired_inline`; retire the 16-frame gate.
4. HUD consistency statistic, log only.

Optional, unchanged in substance: lineage extended to the presented buffer (contribution), the three-way capture comparison tool, PSV0 parsing, raster-state shadowing for MSAA-depth inline.

Ready to build on Cort's word; Astra owns the regressions named above. The SER fix (d6af504) is independent of this and should be confirmed in a run first.
