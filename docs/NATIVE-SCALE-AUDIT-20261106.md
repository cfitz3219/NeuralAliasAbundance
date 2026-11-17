# Native render-scale route: skeptic audit (2026-11-06)

Audit of Sol's 8edb0bc (`codex/sniper-native-scale-control`), merged into the SE5
build as d7dd453 with fixes in 0f0640e. Cort asked for every way it can fail.

## Bottom line

The idea is sound and is the first one that avoids every earlier failure. The game
resizes its own scene, so Windowed, Borderless and Fullscreen are all handled the
same way. Earlier attempts failed on exactly this point:

- fake window sizes
- render-small proxies
- the projection window
- the settings-file trick, which only worked at start-up

The static checks pass on Cort's PC:

- the executable is the same build Sol inspected
- every address is found
- her 20 test cases pass

**It has never run in the game.** The tests use a fake engine. Several things can
only be proven by one real run, and the run log now records each of them.

## The idea in plain words

1. At start, find SE5's own Render Scale setting in its executable. The search uses
   the setting's name and the code that reads it, not fixed addresses.
2. Hook the game's own settings update. When Cort picks Quality, the game is told
   "scale 0.667, upscaler off" just for its apply call. The game rebuilds its own
   scene smaller, through its normal code. The user's own settings are put back
   right after the call.
3. The game then upscales the small scene to full size with its own pass. J2 finds
   that pass and writes DLSS into its output, right after it and before the HUD.

## Walkthrough: from launch to Quality

| Step | What happens | What could go wrong |
|---|---|---|
| DLL loads | `native_scale=1` → locate → hook the settings tick and apply | F1, F2 |
| Menu / first frames | J2 runs DLAA at full size; the first J2 frame arms the control (`seen`) | — |
| Settings tick | Applies the saved mode (from the get-go, no restart) | F3, F4, F5, F6 |
| Game rebuilds scene | Scene textures are recreated at 67%; discovery re-selects them (ratio allowance 1/3) | F9, F17 |
| Each frame | The pass that reads the small scene and writes a full-size target gets DLSS written into its output | F10–F16, F18 |
| HUD | The game draws it afterwards at full size | F20 |
| Window mode change | The engine recomputes the size from its cached scale, so the scene stays reduced | F21 |
| NeuralAlias off | The game's own scale is restored | — (tested) |

## Ways it can fail

The Status column uses these terms:

- **Fixed:** changed in 0f0640e.
- **Logged:** the next run's `native_bridge` log line says whether it happened.
- **Open:** unresolved; needs a run or a decision.

### A. Quality does nothing (the game stays full size)

| # | Failure | What you'd see | Status |
|---|---|---|---|
| F1 | Locator can't find the setting (game patched) | No Quality/Balanced/Performance in our menu; boot log "settings relationship unavailable" | Checked on PC: same exe, found |
| F2 | Hooking the exe trips its protection | Crash or freeze at start | Open: first run |
| F3 | The settings tick never runs after J2 starts | Status stays "Waiting for the game's native settings update"; `ticks=0` | Logged |
| F4 | The tick runs on changing worker threads. Sol's fixed "owner thread" then let the game's own apply on another thread resize back to 100%, and ours forward again. | Constant hitching, flicker between sizes | **Fixed:** override on any thread, one tick at a time |
| F5 | Engine clamps or refuses a scale (Ultra Performance 33% is below the menu's 50% minimum) | A rebuild every tick, so heavy stutter | **Fixed:** 30 tries, then one every 300 ticks; status names the scale it kept |
| F6 | The engine defers the commit (its "inhibit" flag, loading screens) | A short delay | Handled by Sol's retry, now bounded |
| F7 | The game calls its apply every tick (our cache ≠ its menu value) | Small CPU cost | Logged (`game_applies`) |
| F8 | The game saves GraphicsOptions.ini inside its apply | The ini would keep 0.667 | Open: compare the ini before and after the first run |

### B. The game gets smaller but DLSS doesn't run (blurry)

| # | Failure | What you'd see | Status |
|---|---|---|---|
| F9 | Engine keeps full-size textures and draws into a corner | Status "DLSS not reconstructing yet: … not exactly the game's reported scene size" | Logged (`source_size`, `src=`) |
| F10 | **Biggest risk:** SE5 upscales after tone mapping, into a different format | "…the full-size output … has another format" | Logged (`no_destination`, `full_target=` shows the format). The fix needs a conversion step, built once the log shows the shape. |
| F11 | The pass writes two full-size targets of the same format | "…several full-size targets" | Logged (`ambiguous`) |
| F12 | The pass writes more than one render target (output + history) | Was rejected | **Fixed:** allowed when exactly one full-size target matches |
| F13 | Another pass reads the scene first (bloom, luminance) and spent the one-shot hint | DLSS never fires | **Fixed:** the hint lasts up to 24 operations; each must really read the scene |
| F14 | A reused command list carries a stale hint | A wrong target is possible | **Fixed:** the hint is tied to its recording |
| F15 | Upscale pass uses depth, blending or a partial viewport or scissor | Rejected | Logged (`draw_state`, `viewport`) |
| F16 | Upscale is a compute pass | — | Supported |
| F17 | Discovery loses the scene at the smaller size | A few frames without DLSS after each change | Sol raised the allowance to 1/3; logged |
| F18 | DLSS is recorded but the target's state is unknown | Falls back to DLAA on the small image (no visible gain) | Logged (`record_failed`, J2's upscale note) |

### C. Side effects

| # | Failure | Status |
|---|---|---|
| F19 | Sol's frame hook forced the mip bias to 0 every frame. That broke DLAA's −1 and the +4 check, and queued a sampler rebuild every frame. | **Fixed:** set only on a change, and DLAA keeps the menu total |
| F20 | Our HUD routes still run. With a native-size HUD they may composite something they shouldn't. | Open: watch the HUD log lines on the first run |
| F21 | A window-mode switch while at Quality | Expected fine (the engine re-derives the size from its cache); first run |
| F23 | **Not universal:** the locator knows Rebellion's settings code (SE5 and Resistance) only | This conflicts with the universal-only rule; Cort's call |
| F24 | Never run in the game | — |

## If Quality "doesn't respond", the likely reasons in order

1. F10 (format after tone mapping)
2. F9 (no reallocation)
3. F3 (tick not running)
4. F2 (protection)
5. F5 (refusal; only for Ultra Performance)

The status line under Render mode now names the reason. The run log has a
`native_bridge` line every 300 frames with all the counters.

## Fixes in this build

| Commit | Change |
|---|---|
| d7dd453 | Merge of 8edb0bc onto 3b16d38. It was clean; flow snap 0.26 and draw replay are kept. |
| 0f0640e | F4, F5, F12, F13, F14 and F19, plus the `native_bridge` status line and run-log event, the reason shown under Render mode, and tests for the retry limit and the other-thread apply |
| bdd0fbd | XeGTAO temporal stability (separate issue, below) |

The package must carry `[j2] native_scale=1` and `[layer2] enabled=0` (Sol's
`scripts/prepare_se5_native_scale.py` sets both).

## XeGTAO edge shimmer (same build)

Measured on Cort's capture: on still geometry, the AO changes about 9x more between
frames with different jitter than between frames with the same jitter. The cause
has three parts:

- The sample pattern is fixed to the screen.
- The depth is jittered.
- There is no history.

So the contact shadows at an edge flip a little every frame, and more when the
camera moves.

The fix is a new temporal pass:

- It reprojects the previous AO with the same motion vectors DLSS gets, plus the
  change in jitter.
- It keeps only history from the same surface, by matching view depth within one
  pixel.
- It clamps the result to the current neighbourhood so moving shadows don't smear.
- It resets on camera cuts.

There is a new slider, "Temporal stability" (default 0.85; 0 = old behaviour). A
test checks that edge flicker drops by more than half.

Measured on Cort's captures (AO term before DLSS, /255):

- Pan capture: edge flicker falls from 8.23 to 4.57, and swings of 32 or more fall
  from 6.62% to 1.76%.
- Still camera: edge flicker falls from 3.40 to 2.28.

The 3x3 clamp is widened by half its range; that was variant C of the five tested.
Removing the clamp entirely would gain a little more, but makes moving shadows lag
32% more.

A separate finding: the pan capture also shows two DLSS history resets mid-pan, and
a few near-zero-motion frames. Either can cause edge shimmer without AO. Comparing
with XeGTAO off during a pan would separate the two.
