# Automatic GPU backend and reciprocal review — 5 November 2026 Pacific

## User scope and outcome

Cort requested removal of the NVRHI overlay option and automatic use only when it saves overhead, plus a review of Opus's latest fixes. No game launch, game input, deployment, driver-profile writes or installed-game changes were performed. Opus retains the combined production installer/CMD; Cort retains game testing.

The option and its persisted setting are removed. Old experimental/nvrhi keys are ignored and deleted on the next normal settings save. The current automatic choice is the existing native backend: measurement found the present NVRHI wrapper slower, so forcing it on would violate the conditional request. The NVRHI implementation remains compiled and exercised through an explicit test-only backend override. Shared D3D12MA backing and bounded Taskflow preparation remain active independently. No new live calibration loop or per-game exception was introduced.

## Actual backend comparison

`donut_test --benchmark` measures the existing Bloom implementation on an RTX 5070 Ti, 640x360 floating-point input, two warmed passes, nine alternating paired batches, eight warm-up and 32 measured frames per batch. Timing covers CPU preparation, allocator/list reset or open, recording, close and submission. GPU completion waits and readback are outside timing. A second paired comparison isolates the command-list/submission wrapper used by existing native commands.

| CPU interval, median per batch | Native | NVRHI |
| --- | ---: | ---: |
| Actual Bloom prepare + record + submit | 21.628 us | 29.066 us |
| Native-command record + submit wrapper | 2.303 us | 3.931 us |

Both Bloom outputs match exactly (maximum component error 0); service GPU errors 0 and healthy device. An earlier same-session Bloom comparison was 27.122 vs 38.541 us, also favouring native. Absolute timings vary with system load; these measurements establish neither a game FPS gain nor a GPU shader-time difference. They do establish that the wrapper did not reduce these tested CPU intervals. Current native code already pools allocators/descriptors and shares the same physical backing; wrapping that work does not remove it.

Raw evidence, ignored under main `local/opus-af-watch/`: `nvrhi-benchmark-rtx.log`, `nvrhi-benchmark-final.log`.

## Opus placement repair reviewed

Integrated branch `claude/se5-overhead-argon` through af86f31, including 767f7ab. The base consumer identity remains useful when the last mesh changes. The additional tier distinguishes the recording-local last colour writer and entry ordinal; evidence of that same context appearing mid-scene remains disqualifying. This addresses the specific consumer-key collision identified in c741d5c without clearing its contradictory evidence.

Replaced the regression's duplicate handwritten context calculation with actual `InsertionTrace::SetEntryContext` calls and actual `ColorEvents` submission export. It resets between split recordings, interleaves an unrelated resource, and trains from the exported sequence. Results over 60 alternating split/merged/late-water frames: zero early placements; 38/41 split, 9/12 merged, 4/7 water placed after their three learning examples; 51 total eligible placements. Existing indirect/real-entry and trace checks pass. No source change to Opus's placement algorithm was needed.

This approves the demonstrated recording-context repair. It is not a claim that Sniper's visual flicker is eliminated. A context that sometimes precedes a later scene write remains ambiguous and refused; source learning cannot predict an unobserved future draw with certainty. Cort's next game run supplies that visual/coverage result.

## Reproduced mip preparation defect and repair

On the unmodified af86f31 worker, a real hooked compute root/pipeline fixture requests -0.5 bias, waits until requested==live, then performs no draw, bind or deliberate missing-variant lookup. After three seconds `warmed_levels` remains 0 and the new assertion fails. The worker publishes its chosen variant and returns to a wait whose predicate is false; the warm-up block executes only on a later refill notification. Successful activation itself never schedules it.

Now publishing a selected variant schedules one finite idle refill. Initial original/Off can prepare alternatives too. Enabling warm-up wakes it, and new pipeline recipes notify only after registry publication, avoiding a notification consumed before the recipe exists. Refill rate limiting remains, but a different requested choice interrupts that wait immediately instead of waiting a compulsory one second. There is no foreground pipeline compilation or new per-frame scan. The exact no-further-draw test now reports before=0, after=2, want=live=-0.5, passes on RTX and WARP, and is registered as `host_j2_mip_prewarm_test` in CTest. Normal AF pixel tests disable unrelated prewarming for deterministic scope.

Raw `mip-prewarm-before.log` is an intentional failing reproduction; `mip-prewarm-after.log` is the repaired result. Do not report the reproduction as a passing test or as proof of the game's texture-sampling path. Opus's unsaved +4 bias check remains useful to distinguish an actually sampled twin from an untouched/explicit-LOD material.

## Useful findings from Opus's review of our HUD/AF increment

- A verified HUD route now survives HUD-free scenes. Once-per-frame existing draw statistics distinguish absence from missing receipts; 1,000 HUD-absent frames preserve the verified method. Observed HUD without receipts still times out and nonempty failures still advance. Four failed acquisition routes remain Off until real device/queue/extent/format context changes, as Cort requested; no timed retry was added.
- Removed AF driver-profile apply/restore buttons from the overlay. The live `ForcedNow()` value restores the requested grey selector and amber advice. A stale startup override cannot disable it. No profile writes were executed.
- Retained one default-on recovery setting: `[present] hud_separation=0` disables automatic separation at startup while ordinary DLSS remains enabled. There is no HUD overlay toggle or pink mode. Default is 1; existing success receipts and DLSS-preserving fallback remain.

## Qualification and packaging

Complete NVRHI-on Release build passes. Focused 21-case RTX/default CTest run passes, 0 failures, 46.22 s: CPU trace/context, GPU services, Bloom native/NVRHI and integration forms, settings migration, overlay, NR queue/NVRHI, native/counterfactual HUD, J2 native/NVRHI, direct/indirect/real post-entry. Separate registered mip test and benchmark pass. Forced WARP six-case follow-up passes, 0 failures, 14.42 s, including the new mip reproduction repair, both HUD compositors, services and NVRHI paths.

These are automated actual DX12 tests, with fake NGX in explicitly scoped routing fixtures and one existing real post-entry check. They are not live Sniper picture-quality evidence. Regression DLLs use the host-stub adapter and must not be copied into a game. Opus must use the established production adapter and reversible additive package; retain all previous core/jitter/flow/menu-crash fixes and user settings. No new library/effect from the research request was added to this runtime increment.

Final same-source AF pixel recheck: actual hooked WRAP andCLAMP0/4/8/16/0PASS, x16obliquecontrast0.5 vsOff0.001961, isotropiccontrol0change, originalOffexact. Rawaf-review-{wrap,clamp}.log. FinalsoftwaredeviceconfirmedWARP inLastTest.log. Onlycommentanddocumentationchangesafterlastbuild; no runtimechangesafterchecks.
