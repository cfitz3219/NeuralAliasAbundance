# Sol review: split-list delivery and automatic total mip bias

5 November 2026 Pacific. Continues PR111 on `astra/se5-unblend-review`, from d8a61c3 and Sol/Claude af86f31. Cort retains game testing. No game launch, game control, installed-file change or driver-profile write occurred.

## ShaderMake handoff

Posted a concrete assignment to Sol in main COMMS, commit53cc59f: use pinned MIT ShaderMake as a build tool, preserve FXC/DXBC for existing cs_5_0 programs and all shader math/flags/layouts, extract one HLSL source per owned program and embed generated arrays in the existing PSO creation sites. Start with fixed RCAS/jitter-shift/stretch/debug programs; retain already build-compiled Donut/XeGTAO/AMD flow paths, existing background PSO preparation, allocator and ownership. Test clean/incremental/dependency rebuilds, SDK/no-SDK/Linux builds and actual GPU results. No runtime compiler service, new effect or overlay option. No AMD UI composition or new HUD work. Sol owns that implementation; it is not part of this source increment.

## A demonstrated gap in the on/off repair

Sol's767f7ab contextual learner is useful and retained. The earlier CPU test proved context recognition, but its execution path still missed a terminal post pass in another recording when its colour descriptor was GPU-indexed beyond the observer's bounded descriptor window.

The new actual DX12 `--entry-split` fixture uses the existing camera/depth/array-colour case and same terminal shader. It alternates merged scene/post recordings and separate post recordings. Its initial forced-inline reproduction demonstrated the missing frames; the final comparable baseline uses actual Auto routing and a stable final producer so route learning is not confounded by deliberately changing every writer. That unchanged runtime delivers DLAA's black depth-debug pixels on merged frames and raw `3266/34cc/3666` on split frames: only8/17 warmed downstream pixel checks pass. This is an actual routing/evaluation failure, not a changing status label.

Two missing links:

1. `before_read_transition` required scene/depth subresource metadata in the current list before recording the handoff. A pure post list has no scene draw, so its exact whole-resource transition and first shader entry were discarded. Now exact ALL_SUBRESOURCES handoffs are recorded without inventing a selected mip/slice or depth state. Existing local-scene checks still qualify inline insertion.
2. Inline insertion correctly requires that recording's scene metadata, but complementary `record_between_consumer` ignored entry events. It now accepts only an entry already qualified by the existing profile and reuses its existing same-frame `waiting_scene`: colour/depth identity, camera, jitter, subresources and exact submitted-prefix state. The ordinary resolved-consumer path is preserved. Unknown/mid-scene entries do not become colour reads by assumption.

Actual capture metadata identifies this entry too. The existing frame guard prevents inline and between-list evaluation of the same frame. No history is committed merely on recording; existing submission/fence/owner machinery remains. No GPU readback or wait was added to live callbacks. The test's waits/readback are confined to its standalone executable.

After repair, direct AND indirect alternating cases pass17/17 warmed downstream pixel checks on RTX and WARP;21evaluations over24frames after learning, zero late writes and one evaluation per warmed frame. Depth uses inherited or submitted-prefix evidence, with no assumed state or supplied replacement. Existing inline/copy/compute/record-first/auto routes and real-NGX post-entry checks remain qualified by the focused runs.

This fixes the reproduced missing executor/capture links. It does not assert live Sniper visual success before Cort tests the combined production package. Exact-subresource handoffs without the necessary selected-view metadata retain the existing conservative behavior; the added case uses an exact whole-resource handoff.

## Automatic total bias, incorporating Cort's final directions

Normal operation targets the TOTAL sampler bias, not an additional offset:

| Mode | Total |
| --- | ---: |
| DLAA | -1.00 |
| Quality | -1.5849625 (display -1.58) |
| Balanced | -2.00 |
| Performance | -2.25 |

For example, native-1 to Quality changes by-0.5849625; native0 changes by-1.5849625. Native-1 to DLAA is unchanged. The target is exact even if native is more negative, per Cort's "-1 always" clarification; it no longer preserves an arbitrary different total. Point/comparison/data sampler exclusions remain.

Old persisted Off/Light values do not disable the automatic DLAA total at initialization or mode commit. Recovery mode still starts with no adjustment. Existing mode changes apply the existing mode totals live; no restart. The overlay replaces the mip selector, including the greyed-out DLSS dropdown, with **Texture mip bias** and a larger heading-weight numeric total. A preparation line distinguishes the target from the still-active value while background variants finish. No new comparison toggle or persisted manual selector remains; low-level comparison APIs exist only for offline tests.

The new GPU test reuses the SAME texture/root/pipeline while switching0,-1,-0.5,Quality,Balanced,Performance,+4,0. It tests native0, native-1 and native-1.5. Mip levels contain distinct known values; sampled pixels match each expected selected mip (including minimum-LOD clamp), direct Texture.Load control remains unchanged, and restoring the original sampler restores exact pixels. For native0, the basic means are0.219608 at0,0.141176 at-1,0.180392 at-0.5 and0.533333 at+4. Native-1 already equals DLAA's total, so changing to Automatic makes zero difference rather than stacking another-1.

An initial test incorrectly used SampleLevel as its supposedly unaffected control; actual RTX pixels disproved that assumption. Corrected to sampler-independent Texture.Load, and told Sol not to infer sampler bypass from SampleGrad/SampleLevel names alone. These tests establish sampling and routing, not temporal DLSS image quality.

## CI correction and qualification

PR111's original CI42169353537: Linux and no-SDK Windows passed; public-SDK Windows built successfully but one old MSAA shape assertion raced the newly working background prewarm while comparing global twin counts. Demand-compilation tests now explicitly disable unrelated prewarm. The separate registered prewarm test enables and verifies it; no runtime feature was disabled to hide a failure.

Complete Release build passes after the final automatic-total/readout changes. Final focused15cases pass32.62s, including initialization/recovery, menu, all three native-bias pixel cases, direct/indirect split entries, mip prewarm, MSAA and real post-entry. Prior broader18affected checks before the last automatic-total changes pass44.89s. Forced WARP follow-up and any broader/CI results are appended to main COMMS on completion. No game quality/FPS claim is made from fake-NGX fixture checks.

Raw ignored evidence: main/local/opus-af-watch/entry-split-{before,auto-before,auto-after}.log; mip-pixels-{initial,final-rtx,final-warp}.log; automatic-total-final-{build,tests,warp}.log. Initial failing/intermediate test assertions are preserved there, not reported as passes.

Opus/Sol must integrate the completed source into his ShaderMake/production branch and build the established reversible additive package. Regression host-stub DLLs are not an install payload. Preserve core, jitter, flow, four-route HUD fallback, D3D12MA and Taskflow. The AF selector remains live unless a fresh driver override is active, with Cort's exact amber wording and no profile-write buttons.

Final forced-WARP7/7PASS18.86s includes all native-bias pixel cases, both split entry forms, prewarm and the formerly racingMSAA case. No runtime changes after these checks (only explanatory comment/receipt additions). Broad qualification and refreshed CI follow this completed source increment.
