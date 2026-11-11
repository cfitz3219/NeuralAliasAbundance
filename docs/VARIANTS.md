# Variants register

When two games need different behaviour for a real reason, the program keeps both
(Cort, 2026-11-11: "everything we have is there for a reason"). This file is how
Claude, Astra and Sol know which is which. Nobody carries memory between sessions,
so the register lives with the code.

## Rules

1. A variant is chosen by something the program observes in the running game.
   Never by a game name or an exe name. That is what lets a third game use it.
2. Each kept variant has one row here.
3. The code that chooses it carries a comment `VARIANT: <name>` with the same name.
4. The status log names the chosen variant, so one run shows which path a game took.
5. Each row names the test or saved recording that proves the variant still works.
6. `scripts/check_variants.py` runs in CI. It fails when a `VARIANT:` comment has no
   row, when a row's test file does not exist, or when a row is missing a field.
7. A variant that could not be kept, or whose choice changes what Cort sees, is a
   question for Cort, with the evidence. It is not settled here.

"First seen in" records where the behaviour came from. It is history, not a selector.

## Register

Columns: name | what it does | first seen in | observed condition that selects it | proof | log field | arrived in | owner

| Name | What it does | First seen in | Selected when | Proof | Log field | Arrived in | Owner |
|---|---|---|---|---|---|---|---|
| scene-end/entry | Starts DLAA at the operation that has been the first one after the finished scene on earlier frames | Sniper Elite 5 | The operation is one the entry profile has learned and accepts | tests/cpu/post_entry_test.cpp | `j2_entry matched` | f1ca3e5 | Astra |
| scene-end/class | Starts DLAA at the real consumer of the scene picture when its kind of reader is accepted | Anno 1800 | No accepted entry; the consumer's class is accepted by the learned consumer profile | tests/cpu/consumer_profile_test.cpp | `class_matched` | e19f7a4 | Claude |
| scene-end/pair | Same as scene-end/class, accepted for a known producer and consumer pair | Anno 1800 | No accepted entry; the class is accepted only together with its producer | tests/cpu/consumer_profile_test.cpp | `class_pair_matched` | e19f7a4 | Claude |
| scene-end/barrier | Starts DLAA at the resource barrier that has ended the scene on earlier frames | Anno 1800 | No accepted entry or class; the barrier is the learned scene completion for this final producer | tests/host/overlay_boundary_fallback_test.cpp | `barrier_matched` | cc93a6e | Claude |
| scene-end/boundary | The original boundary matcher on the scene picture's last write | Anno 1800 | None of the learned kinds has an answer yet | tests/host/overlay_boundary_fallback_test.cpp | `legacy_matched` | cc93a6e | Claude |
| camera12/joined | Applies jitter to a constant block that holds the whole camera matrix | Anno 1800 | The block contains the known combined camera | tests/host/j2_anno_shape_test.cpp | `jitter_writes` | aca551b | Astra |
| camera12/parts | Applies jitter to a block that holds only one part of the camera (lens, inverse, or previous frame) | Anno 1800 | The block contains a known camera part and no combined camera beside it | tests/host/j2_anno_shape_test.cpp | `jitter_writes`, `other_camera_blocks` | aca551b | Astra |
| camera11/packed | Reads the camera from one packed constant block using a known layout | Call of Duty WWII | A size-matching layout passes its checks on this frame | tests/host/native11/provider_test.cpp | `pose{native, legacy}` | 15fc57c | Astra |
| camera11/split | Builds the camera from two small blocks that agree across many draws: per-object translation and the view-projection matrix | Call of Duty WWII | Viewport jitter owns raster positioning and the two blocks agree across the frame's draws | tests/host/native11/provider_test.cpp | `pose{split}` | fc0e6bd | Astra |

## Not carried, and why

Kept on their branches, untouched. Listed so nobody re-merges them by accident.

| What | Where it lives | Why it is not in the combined build |
|---|---|---|
| Sniper predicated and exact placement | `astra/se5-segment-placement`, `claude/project-thread-pfxnx8` (481dfc4) | Made flicker much worse in the game; Cort stepped back to 6f6b806 (2026-11-02) |
| Sniper raster-jitter and scene-ending stack | `astra/se5-pre-exact-pan`, `astra/se5-residual-misses-flow` | Rejected in the game; Cort directed a return to the first low-flicker build (2026-11-03). scene-end/entry replaced it |
| Signed lens matrices (bd5fd04) | `astra/se5-pre-exact-pan` | Parked. A general camera rule worth bringing back with the general camera finder, with its fixture |
| Pre-frame-generation inferred states (e53d2b3) | `claude/nr-inferred-states` | Failed review on a real GPU barrier error (2026-10-29) |
| Early DX11 host and bridge, DX9, Vulkan layer | `claude/project-thread-2b5ysj` | Other graphics APIs; outside the Route B DLAA consolidation. The native DX11 provider replaced the early DX11 host |
