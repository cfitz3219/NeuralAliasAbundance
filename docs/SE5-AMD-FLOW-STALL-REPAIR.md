# SE5 AMD flow stall repair

Cort's SE5 run at source4d2edca reached213recorded/submitted frames and88pairs, then stayed there from16:36:30 through16:38:21 while resource-busy skips increased from212 to4849. The displayed0.23% and0.876ms were the unchanged last completed pixel sample. Evidence is preserved in local/se5-amd-stall-20261101 with a hashed source-log summary.

## Cause and correction

AMD descriptor slots were selected by J2's six-position frame-ring index, while their ownership lasted through the game's executable command-list recording. These are different lifetimes. A closed list can remain executable after its GPU work has completed. The old fixed eight-image/descriptor pool could therefore run out even though the GPU was idle. A GPU regression retains old closed recordings and reproduces the failure on recording7.

The correction assigns descriptor/readback/query storage to actual recordings, using the existing recording lease. Storage is pooled and reused after retirement; another recording can use a different entry. Encoded history images likewise remain intact for their producers/consumers and the pool can accommodate retained recordings. The existing retention service bounds outstanding recordings. This does not release or overwrite an old executable list's resources to make room.

AMD's search shaders, cached PSOs, colour encoding, residual math and acceptance thresholds are unchanged. The same-queue working scratch keeps its existing explicit start/end states. The three existing helpers accept optional recording-owned descriptor storage; their default callers preserve the prior behavior. The correction also retains colour/depth inputs for executable recordings.

GPU validation found a separate host API error: AMD ClearUAV's CPU handle pointed into shader-visible memory. It now uses a matching typed descriptor in a non-shader-visible CPU heap, with the GPU descriptor in the recording's heap. No vendor shader source was changed.

## Status display

The overlay shows paired/submitted frames and skips separately from the last completed pixel sample. It labels that sample's frame and lag. When submissions stop, it says there are no new submissions and omits a misleading current percentage/GPU time. Invalid/depth, small residual, image/flat and offscreen exclusions are available separately. Logs retain all six pixel counters, submission/sample ages and pool sizes. Late retirement of an older recording cannot replace a newer displayed sample; reset clears the previous sample.

The percentage describes pixels where extra moving-object motion was accepted. It is not a percentage of processed frames, and a small value alone does not establish bad flow. Camera-derived motion remains responsible for the static scene. Actual moving-object benefit remains a visual test.

## Automated verification and delivery

The new regression retains24closed recordings, continues for256frames on a normally reset active list, checks280submissions/279pairs with no busy skips and stable pool size, then replays the old command lists and compares their numerical outputs with their original results. It uses observed Reset and actual GPU completion; no manual lifetime shortcut. DX12 debug-layer errors fail the test. It also checks telemetry freshness, rejection-counter totals and reset behavior. The original numerical moving-object test remains separate.

The full build/test and package receipts are recorded in WORKLOG/COMMS after completion. All evidence here is automated; no SE5 game test is claimed.

The prepared package reuses the latest SE5 Results/rollback transaction and captures. The normal SE5 DLAA, flow and camfix update/rollback/results commands point at the same verified payload, with previous launchers backed up. No Resistance launcher changes. The agent does not install or launch either game; Cort tests with SE5 closed before running the update.

References: [Microsoft descriptor synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptor-heaps-overview), [command-list reuse and reset](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles).
