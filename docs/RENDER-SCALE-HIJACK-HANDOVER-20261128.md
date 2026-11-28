# Render-scale hijack: handover to Astra (2026-11-28)

**Later the same day:** Astra answered with the renderer-side route (PR 210), and Cort ordered the memory-search code removed. The scale witness, the Search, the writing trial, the writer watch and their tests were deleted; the last commit that has them is `a337fe3b`. This note is kept as the record of what was tried.

From Fable (Claude). Cort's order, 2026-11-28 10:13Z, verbatim:

> Give it to Astra. Tell her the goal, everything you built, everythng you tried, what failed, why it failed, and your estimation of the problem. Then ask it to solve it. Ask it to solve it. Whether directly, or clevery. This hasn't been done so we need an idea that is totally unique to get this solved in a way that doesn't require per game patches.

**The ask: solve it.** Directly or cleverly. Everything below is what I know; none of it binds your approach.

Code: branch `claude/project-thread-onxwhw`, draft PR 208, head `a337fe3b` (all CI green). Main file `src/host_dx12/scale/scale_witness.cpp`, pure logic in `src/core/scale_hunt.hpp`.

## 1. The goal

Real upscaling (Quality 67% / Balanced 58% / Performance 50%) on games where NeuralAlias adds DLSS itself (Route B), by making **the game** draw its 3D picture small, with its interface still at full size, and running DLSS from the small picture to the screen. The Fold (coarse shading) cannot do Balanced and its gain is small on some games; that is why Cort reopened this on 2026-11-28.

Cort's named chain (use these names in logs, RESULTS and menus):

1. **The Search**: at launch, hands-free, NeuralAlias finds how to make the game render small.
2. **The Connection**: only after the game is confirmed to respond: DLSS on the small picture; Quality / Balanced / Performance drive it.
3. **The Fold**: fallback when the Search fails; Balanced grayed out.

Hard rules that shaped everything (all Cort's):

- Universal only: no game names in code, no per-game patches or tables, no settings-file substitution.
- No player steps: no slider moves, no "learn on run 1, apply on run 2", no restarts. First launch must work.
- Works in Windowed, Borderless and Fullscreen; DX11 and DX12 alike.
- Never drops to DLAA silently; every log says WHY.
- No anti-cheat games (Cort: not a target).
- He rejected "shrinking the game from outside" on 2026-11-25 ("We walk away from the Astra idea for a reason. It broke things constantly"). That was your render-small work; you know its failure modes better than I do. He has not withdrawn that, but he has now asked for a totally new idea, so say plainly if your answer revisits it and why it would not break this time.

## 2. Test game and its facts

Crusader Kings III (`binaries\ck3.exe`, DX11, SDL window, Jomini/Clausewitz). No DLSS in its menu; it has a "Render Scale" slider that applies **live** (no restart). Cort plays 3440x1440 in **true fullscreen** (`display_mode "fullscreen"` in pdx_settings.txt).

- Slider test (build d0802dc9, Cort moved the slider): 3D world went 3440x1440 -> 2408x1008 (0.70) -> 1720x720 (0.50) -> back; output stayed 3440x1440. So the game uses smaller render targets and upscales them itself.
- Numbers that followed the slider 2 of 2 (found by memory search only): three floats 16 bytes apart on the heap (`...160d0/e0/f0` that launch) and one float 8 bytes after a size pair (`...8398`). Heap addresses differ every launch.
- Names in the exe: `render_scale`, `SETTING_RENDER_SCALE`, `Mesh.ScreenPercentage`, and a whole family `Upscale.LodBias`, `Upscale.Reset`, `Upscale.Sharpening`, `Upscale.SharpeningFactor`, `Upscale.AutoExposure`. The game ships unused `nvngx_dlss.dll` 2.4.0.0 and `libxess.dll`, and pdx_settings.txt has `upscale "DISABLED"` and `upscale_quality "off"`. So the engine has a dormant upscaler path of its own that the menu does not expose. (Interesting, but game-specific.)
- One candidate from code: `ck3.exe+0x5d40330` (value 0.5 while the game was at 1.0); never confirmed.

## 3. What I built (all in PR 208)

| Piece | What it does | State |
|---|---|---|
| Scale witness, five finders on one board | call chain, target creation, memory sweep, setting names in the exe, pointer following; `ResponseJudge` with hold policy | works; confirms numbers only after the player moves the slider |
| Writing trial (`[scale] trial=1`) | writes each confirmed number by 5 points, judges by the live world size, restores, finds pointer routes from the exe | proven on a pretend game; never reached in CK3 without slider moves |
| Writer watch | DR0-3 on all threads + vectored handler; names the game code that writes a confirmed number, unwound from the exception context | built, 64-bit, log only; never fired in CK3 (needs a slider move after confirmation) |
| **The Search** (`[scale] test=1 search=1`) | hands-free: candidates that hold the starting value (measured world/output) by clue: numbers read by name-using code; objects carrying a precise render-scale name (text or pointer to text); numbers beside the 3D picture's size; loose names last. 5-point nudge in groups: whole, bit-position splits (parts a setting from its applied copy), halving, winner alone three times | proven on pretend games; in CK3 it FOUND the `render_scale` object but the game did not respond |
| Write-then-signal | after plain nudges fail: write a group, then `do_poke`: posted WM_SIZE + WM_EXITSIZEMOVE; posted WM_DISPLAYCHANGE; real SetWindowPos height-8 and back | proven on pretend games; no response in CK3 |
| Handle | `witness::ControlReady()` / `RequestScale(ratio)` holds the game at a size, re-pokes if a poke was needed | proven on pretend games at 66.7 / 58 / 50% |
| Tests | `scale_search_test` (default, `--copy`, `--apply-message`, `--apply-resize`), `scale_trial_test`, `scale_witness_test`, `na.cpu.scale_hunt` | all pass in CI and on Cort's PC |
| By-products worth keeping | first-launch freeze fix (`compute_flow::Warm`, DX11 set-up thread, `core/first_setup.hpp`); NeuralAlias's own DLSS file in `NeuralAlias-DLSS\` | proven on Cort's RTX without a game |

Not built: The Connection itself. Map of what it needs (from a code survey): native DX11 DLAA assumes output == input (`native_d3d11/live_dlaa.cpp`: `Prepare` makes the output from the colour description; `eval.render_w = eval.out_w`; depth must equal the active colour size) and writes the result back into the game's own colour texture. DX12 already has `UpscaleTarget` / `SetUpscaleTarget` (`j2/live_dlaa.hpp`), the projection window, render-small at Present and the native scale bridge. `AddedUpscalingAllowed()` in `settings.hpp` is hard-coded false and gates `native_scale`, `read_hook` and `render_small`.

## 4. What I tried in the game, and what failed

All four CK3 runs were clean: no crash, no hang, 60 fps (one 5 s dip to ~38 fps during the first memory sweep).

| Run (build) | What | Result |
|---|---|---|
| d0802dc9 | finder with Cort moving the slider | PASS: live scale, 4 floats confirmed |
| a2c00a43 | Search v1: 2,703 numbers, two whole-group nudges under 1 s | no response; log could not say why (my miss) |
| c4da367e | Search v2: precise names first, every split, 2.5 s per nudge, 51 nudges | found text `render_scale` on the heap with 3 numbers at 100% within 256 bytes, plus pointers to it; **no response to any nudge** |
| a337fe3b | v2 + write-then-signal on the precise group (8 numbers) and the beside-size group (311) | **no response** to WM_SIZE/WM_EXITSIZEMOVE, WM_DISPLAYCHANGE, or the real resize |

## 5. Why it failed (my estimation)

1. **CK3 does not read its render scale per frame.** The setting object and its copies are data at rest. The slider's handler runs an "apply" step that rebuilds the render targets; nothing else consults the number. Writing memory therefore changes what the menu would show, not what is drawn. I believe this is the common case for engines whose scale lives in a settings menu, and that per-frame readers (dynamic-resolution engines) are the minority. I have one game of evidence.
2. **A fake window message is not a rebuild.** SDL compares sizes and ignores a WM_SIZE that changes nothing; WM_DISPLAYCHANGE did nothing.
3. **The real resize was not a fair test.** In true fullscreen the SetWindowPos did not take hold (the witness saw no size change at all). It may work in borderless or windowed. It still fails Cort's "all three display modes" rule, so I did not chase it.
4. **I do not know that a rebuild re-reads the setting at all.** It may use a cached value.

Things I did NOT try, so they are not ruled out:

- The loose-name group (2,503 numbers) with a signal; the `Upscale.*` objects.
- A hardware READ watch on the three floats, to learn whether any code reads them outside the menu, and when.
- The writer watch with a slider move (Cort declined further slider moves; do not ask him again).
- Calling the game's own apply function. I stopped here on purpose: finding it needs either a slider move per game (breaks "no player steps") or per-game knowledge (breaks "universal").
- Driving the game's own menu by synthetic input (fragile, visible, and a per-game layout).
- Anything at the graphics-API level instead of the game-setting level.

## 6. Where I think the answer may be (untested thinking, take or leave)

The dead end is specific: **hands-free discovery of a per-game apply step.** Routes that avoid needing it:

- **The API is universal where the setting is not.** Every game must create its world targets and set its viewports through D3D. A route that makes the *game's own calls* produce a small world (not by resizing behind its back, which is what broke before, but by controlling what the game believes its output size is while the interface is redirected or re-rasterised at full size) needs no per-game knowledge. The hard part is exactly what broke render-small: interface, post-process passes and mouse mapping that assume one size.
- **Settings are read at start-up in every game.** NeuralAlias is loaded before the game applies its settings. The apply step that I cannot trigger later runs once, for free, at launch, with whatever value the game holds at that moment. A hands-free way to make that value the wanted one without substituting the settings file (Cort's rule) would give one level per launch; switching levels live would still need something else, and Cort requires no restarts.
- **A read watch at the right moment** could turn "which function applies it" into something observed hands-free, if any universal event makes the game run its apply step (device loss and restore? a mode switch the game itself performs? alt-tab in true fullscreen recreates targets in many engines).
- **Use the Fold where it is strong and close its Balanced gap another way** (a third block shape, or a DLSS input built from a mixed-rate picture), if the real goal is "three clearly faster levels" rather than "the game renders small".

## 7. Practical

- Packages on Cort's PC (`Documents\`): `NeuralAlias-CrusaderKingsIII-Search-a337fe3` is **still installed in CK3** (DLSS off); UNINSTALL is in that folder. Results folders `NA-ck3-results-20261128-*`; evidence `%LOCALAPPDATA%\NeuralAlias\ck3-native11-*`.
- Worktree on his PC: `C:\Users\Cort\NA-wt-scale` (build dir `build-c66d14c`). Builds use affinity mask FFFF33CC, never while a game is open, no PIX.
- Known small bug: a package's RESULTS fails if run while the game is still open (witness log is locked); a second run works.
- I told Cort at 10:10Z that under our two-round stopping rule the hijack was shelved and The Fold stays. He then ordered this handover, so the idea is open again, with you.
- Cort was patient through four runs tonight and is tired of rounds that end in "not found". Prove the next idea offline (pretend game, or a capture) before any package, and tell him the coverage and limits before the first package.
- Raw run notes for you on Cort's PC: `C:\Users\Cort\Documents\NA-scale-search-handover-for-Astra.md` (every CK3 run with numbers and log paths). It includes the call chains the slider test recorded when CK3 rebuilt its 3D pictures: the shared creation trunk `ck3.exe+0x4486220 <- +0x443b750 <- +0x43f6010 <- +0x3b01210 <- +0x3b00ab0`, reached from the menu's apply through `+0x3f1fd40 <- +0xa34580 <- +0x3ca1a90` and at start-up through `+0x45156e0 <- +0x4516f90`. Those offsets are one game's and one version's; they are evidence of the shape (one rebuild routine, two callers), not something to put in code.
