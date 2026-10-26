# Scoped camera math handoff — 2026-10-26

Base inspected: Opus `378663e`. Cort assigned the useful specialist tasks from
Argon's `afd5a27` audit; live integration remains Opus's work.

## Delivered API

- `FindCameraOriginCandidates(previous_bytes, current_bytes, relative_vp,
  excluded_ranges, max_translation)` returns all finite, moving float3 candidates
  from the remaining float-aligned slots. Supply consecutive snapshots of the
  same camera block/layout and exclude known matrix ranges. The scan applies
  when the decomposed matrix camera origin is near zero. It does not select a
  winner or identify Anno's actual camera position; smooth vectors are ambiguous.
- `ComposeCameraRelativeViewProj(previous_relative_vp, current_minus_previous_origin)`
  returns `previous_relative_vp * T(delta)` in column-vector convention. Transpose
  for the row-vector motion arbiter. Positive delta is intentional: a point
  relative to the current origin needs that delta added to become previous-relative.
- `CameraTracker::UpdateCameraRelative(m, identified_world_origin, frame, width,
  height, known_jitter_px)` integrates the above into camera history. Current
  inverse remains current-relative; previous VP is expressed in that same space.
  History stores the original previous-relative matrix, not the adjusted result.
  Cuts use world camera position and switching coordinate modes resets history.
- `UnjitterViewProjection(m, x_px, y_px, width, height)` exposes the existing exact
  inverse projection-offset operation. `Update` reuses it. Known injected samples
  preserve an intentional off-axis projection; native jitter estimation remains
  explicitly approximate. No new jitter generator or native-jitter classifier.

Ordinary `Update` callers keep their existing behavior. Opus should call the new
relative API only after identifying the origin from the live camera block. The
helpers add no new discovery gate and perform no game memory writes.

## Verification

CPU regressions compare camera-relative and ordinary world-space clip transforms
to 1e-6 and the actual `ReprojectPixel` output using production float matrices to
0.02 pixel, across both handednesses, normal/reverse depth, pure pans, combined
rotation/translation, known jitter, and camera cuts. The CBV test scans a packed
float3 at +68 and feeds the selected fixture candidate into the tracker; ambiguous
alternatives remain visible. Known jitter removal is checked while the camera
moves and the projection includes a deliberate fixed offset.

Windows Release build and four targeted tests pass: camera_tracker,
host_dlss_backend_test, host_j2_live_test, host_j2_anno_shape_test. Host DLSS uses
fake NGX; this is not an Anno image-quality or origin-identification claim.

## Focused review sent to Opus in COMMS

1. **J1 motion input omission:** `live_dlaa.cpp:548-554` at 378663e fills the GPU
   camera matrices but leaves `Inputs::jitter_current_px` zero. Copy the same
   `cam.jitter_px_x/y` supplied to DLSS into the motion inputs before Dispatch.
   The GPU reprojection subtracts this sample at `motion_arbiter_gpu.cpp:80-81`.
2. **FG resize-timeout lifetime defect:** `presenter.cpp:384-386` can return from
   `release_size_dependent_locked` without draining GPU work. `BeforeResize` at
   757-758 still calls `fg::Shutdown`; `frame_gen.cpp:628-638` releases resources
   and retired features assuming the GPU was drained. Propagate the release
   helper's success and retain FG state on timeout. Source-confirmed conditional
   use-after-release path, not reproduced live; does not block DLAA-only testing.

The inspected J2 successful list restores the color/depth states it transitions,
checks slot and resize fence completion, and owns its command list/descriptor
bindings. Backend feature retirement waits on tracked uses. No additional concrete
defect established in these paths; this is not a blanket concurrency certification.

Audit recipe corrections are posted in COMMS: scene/backbuffer difference is a
heuristic HUD mask, not exact alpha extraction; flow times depth is not general
3D translation recovery; temporal resource bindings do not uniquely identify TAA.
Reuse the already implemented live bridge and known-jitter removal. No added FG
milestone or additional review cycle.
