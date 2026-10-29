# Phantom Blade Zero: Apply and Neural Rendering repair

2026-10-29. Based on Opus's `claude/pbz-additive` at 7ba47a8. Game testing remains with Cort.

## Evidence

The observe log contains successful Ray Reconstruction (NGX feature 13) creations at 1146x480 input, 3440x1440 output and quality value 3 (Ultra Performance). Apply caused creations at 3439x1439 output and then 3440x1440 again. Input stayed 1146x480 even when our override changed the quality value to Performance or DLAA. The game **did rebuild DLSS**; it did not change its internal render scale. The old message confused those events. Even our rounded Ultra Performance request (1152x480) differed from the engine's 1146x480.

The 03:55:04 status record shows NR enabled but zero evaluations/submissions, with the different-queues refusal, two direct queues, and distinct render/presentation queue addresses. Menu GPU work completed. Logs/preferences are preserved in the primary checkout's `local/pbz-repair-20261029/`.

## Repair

PBZ defaults to `follow_game_resolution=1`. Preset overrides preserve the engine's quality value and optimal-size answers. The overlay provides “Use game's resolution” and disables its mode cards while selected. RR Apply uses the existing game-triggered rebuild but confirms the actual current input extent. Rebuild and resolution confirmation are reported separately. Presets remain requests to NVIDIA's runtime, not a claim that a particular model was used. Ultra Performance stays available in the game's settings. **Automatic PBZ engine resolution changes through our menu are not implemented**; changing NGX's quality label cannot change the engine's render targets.

NR inputs use a bounded pool of independent snapshots. Each remains occupied through producer completion and the last NR submission reading it. A published, unconsumed snapshot cannot be reused by the next recording. Each has its own producer fence, so changing producer queues does not require ordering game queues against each other. NR waits on that fence on the presentation queue. Game submissions never wait for NR, avoiding a cycle with an FG queue waiting for a future game submission. Same-queue work uses ordinary queue ordering; pool exhaustion skips capture without a CPU wait. Successful and restore-only failure submissions share the ordering helper. `nr_queue_sync` logs input completions and GPU waits.

Reference: Microsoft's [queue synchronization guidance](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization) and [`ID3D12CommandQueue::Wait`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait). GPU waits return immediately to the CPU; resources retire on completion evidence.

## Verification

Fresh SDK Release build: `build/pbz-queues-20261029`, VS18/MSVC19.51, discovery adapter. Eight focused regressions passed in 5.63s: NR queues, depth conversion, SR/RR create parameters, SR/RR optimal settings, PBZ settings persistence, menu pages, WARP overlay and RTX overlay. Additive installer fixture tests passed as well.

NR regression uses RTX5070Ti, three real DX12 queues and a **fake model**. It checks delayed producer, delayed consumer with continued producer progress, an FG-like future-producer dependency, changed queues, same-queue fast path, stale-input skip, scaled inputs and failure restoration. Seven GPU-read-back depth witnesses match every pixel. Seven evaluations; nine of nine NR command lists completed; nine of nine input snapshots completed; nine cross-queue waits; zero D3D12 debug errors. This validates synchronization/data preservation, not NVIDIA model quality or actual game performance.

First test compilation needed the texture helper's explicit flags argument. An initial two-way shared-buffer fence version passed tests, but review identified the possible FG dependency cycle; independent snapshots replaced it before release, with a new continued-producer-progress assertion. No game received the intermediate version.

## User-run package

Close PBZ, run `Documents/NA-pbz-repair.cmd`, then launch normally. Keep the desired DLSS resolution mode in the game's menu. “Use game's resolution” defaults on. NR can be enabled independently; Apply is only needed for a preset change. Rollback remains `Documents/NA-pbz-rollback.cmd`.

The existing additive deployment mechanism and NVIDIA NR 310.9.16 are reused. Original SR/RR/FG files are untouched. Astra did not launch/deploy the game. Actual PBZ NR behavior with native FG, image quality and performance await Cort's run.
