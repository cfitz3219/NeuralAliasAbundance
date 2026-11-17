# Camera-independent DLAA: first implementation handoff

2026-11-16, Astra. Cort authorizes Fable to implement and Astra to audit. Source inspected: `b36d261488985b96576339e163a31666496d9390`, the current shared integration branch at this audit. No runtime changes, game launch or installation. This is the first numerical/source increment, not approval of an implementation that has not arrived.

## What already works and should be reused

The existing CPU and GPU arbiters accept complete **native** motion without any camera or depth input to arbitration. The scene depth is still required by DLAA itself. New regressions deliberately poison the unused camera matrices and jitter with NaNs rather than supplying identity matrices. They preserve valid zero motion, fractional motion and large motion exactly. Residual snap/clamp settings do not affect the independent source. Missing/invalid samples remain distinguishable from valid stationary samples in provenance. An optional camera disappearing and returning cannot alter valid complete native motion.

The native DX11 FrameModel also selects scene color/depth and the tonemap boundary without a camera binding. The new comparison removes all constant-buffer bindings and still selects the same point through warmup and subsequent frames. This does **not** demonstrate that the live provider emits such a frame: it currently does not. It establishes that its selection machinery need not be replaced to remove that gate.

Do not relabel estimated flow as native/ENGINE simply to use this tested branch. Give estimated total motion its own truthful provenance, preserve its validity, and reuse the arithmetic and normalization contract. Flow estimation and its image-quality acceptance remain to be implemented and tested independently.

## Concrete integration traps to address

| Source at b36d261 | Finding | Required change within the existing path |
|---|---|---|
| `native11/provider.cpp`, scene callback construction around 1032-1150 | CameraNow failure returns before emitting SceneFrame. Later code dereferences camera upload/bytes and obtains reversed depth from `cam.reversed`. | Make camera facts genuinely optional in frame construction; obtain depth convention from observed depth usage/clear evidence. Removing only the backend Pose gate is insufficient and leaving the dereferences is unsafe. |
| `native_d3d11/live_dlaa.cpp::Pose`, `EvaluateFrame` around 242-352 | Pose requires camera bytes and admission precedes flow. CameraFrame also supplies jitter and cut/reset information. | Separate observed raster jitter, frame continuity and motion-source selection from optional pose. Camera-dependent optional effects such as projection-based AO must not become an indirect base-DLAA gate. |
| `native_d3d11/live_dlaa.cpp` around 411-423, 640-672 | Flow's previous frame and reset decision use `s.last_frame`, which advances only after DLAA delivery; the next jitter sample is also scheduled after successful delivery. | Seed and commit the first acquired image independently of DLAA delivery. Track last acquired/submitted flow image separately from last delivered DLAA output. Preserve resource retirement and queue order on this seed-only submission. Otherwise a no-camera start can repeatedly request/reset its first pair and never become ready. Request real raster jitter while acquisition is preparing that pair; do not fabricate a delivered frame. |
| `native11/provider.cpp` around 1226-1235 | Camera buffer identity can change `selection_changed`/stream even when color/depth and the camera-independent source are unchanged. | Keep scene/image history identity independent of unused camera-buffer rotation. A real camera-source/coordinate transition has its own reason; acquisition of an optional camera is not automatically a scene cut. |
| `j2/compute_flow.cpp` around 203-246 and `core/motion_arbiter_gpu.cpp::DispatchDeriveResidual` | Expanded total image motion exists internally; the public result contains a residual. Residual derivation explicitly refuses absent camera validity. | Expose total motion with exact image pair, coordinate domain and validity before camera subtraction. Do not remove the residual path's validity rule or send its residual as total motion. |
| `j2/flow_shaders.hpp::accept` | Existing acceptance rejects residual length below 0.26px, requires improvement over camera-only prediction, and optionally requires prior directional agreement. | Keep these rules for residual correction. A complete-motion validator must admit verified zero motion and slow pans, compare appropriate image hypotheses, and handle real reversals/disocclusion without requiring camera prediction. Do not just raise the looseness slider or reuse its mask unchanged. |
| `j2/live_dlaa.cpp` around 3830-4023 | DX12 camera discovery, prediction and inversion are also prerequisites; motion, depth convention and jitter are subsequently read from the admitted camera. | Carry the same independent facts through this executor. Keep its existing submitted lifetimes and placement; removing only the native11 gate would leave the DX12 requirement unmet. |

These are consequences of extending the current camera-based design. They are not claims that these paths are all bugs in the existing admitted-camera configuration.

## Motion/jitter handoff to preserve

- Output is **total current-to-previous motion in render pixels**, positive right/down. A confidence of 0.25 authorizes a sample according to its validity contract; it does not mean multiplying the displacement by 0.25.
- For flow measured directly on jittered raster images, remove the previous-minus-current **actual image** jitter once. The current AMD path instead stabilizes its input images before estimating flow; those vectors must not have that jitter difference subtracted again.
- Sample/regrid the stabilized field onto the current raster grid once. Existing FlowExpand uses `image_to_raster_px`; preserve that mapping and its inverse when reading depth. A real viewport write does not prove that all final scene layers follow it.
- Every result identifies the actual submitted pair, view/source generation, extent and domain. Failed/discarded recordings are not history. A successful seed-only submission can be history even though it was not a DLAA delivery.
- A valid zero vector and an unavailable vector are different facts. The existing NGX submission does not consume our provenance texture as a validity mask, so the runtime still needs an explicit policy for unreliable areas and cuts; diagnostic provenance alone is not a solution. Do not call whole-frame zero motion validated because a dispatch succeeded.
- Optional camera loss must not reset an unchanged independent path. A true cut, size/coordinate change or different source image requires a deliberate history transition using existing generation machinery.

## Tests added and run

All are existing targets; no new build system or runtime API was introduced.

1. `tests/cpu/motion_arbiter_test.cpp`: three new cases, **13 total passed**. Tests camera absence, poisoned unused matrices, non-tight input stride, stationary/subpixel/large motion, malformed validity/nonfinite vectors, residual-only rejection and optional-camera transitions.
2. `tests/host/camera_fit_gpu_test.cpp`: added actual GPU arbitration with no camera/depth and poisoned unused camera values. On **NVIDIA GeForce RTX 5070 Ti**, all **230,400 pixels** had exact expected vectors and provenance (zero mismatches). Existing camera-fit, corruption and phantom-jitter checks also passed; executable exited with zero failures.
3. `tests/host/native11/frame_model_test.cpp`: added no-camera-binding versus normal-input comparison over eight frames. Full existing frame-model suite passed, including layered/second-depth cases.
4. Related standalone CPU regressions `jitter_domain`, `game_motion`, `camera_fit_basis` passed. Four selected CTest targets including motion_arbiter: **4/4 passed**, Release, MSVC 19.51, warnings as errors.

The GPU test uses deterministic supplied vectors. It does not exercise a new camera-free optical-flow estimator, real DLSS, live scene acquisition, or game quality. It proves the reusable arithmetic seam and preserves the optional camera fitter. No all-games or FPS improvement claim.

Local reproduction from `C:\Users\Cort\NA-camera-independent-audit`:

```powershell
# CMake/CTest are in the Visual Studio 18 Community bundled CMake/bin directory.
cmake -S tests/cpu -B local/camera-independent-cpu -G "Visual Studio 18 2026" -A x64
cmake --build local/camera-independent-cpu --config Release --target na_cpu_motion_arbiter na_cpu_jitter_domain na_cpu_game_motion na_cpu_camera_fit_basis
ctest --test-dir local/camera-independent-cpu -C Release -R '^na.cpu.(motion_arbiter|jitter_domain|game_motion|camera_fit_basis)$' --output-on-failure
```

The shell did not have CMake on PATH; the actual calls used its absolute Visual Studio path. The two host tests were compiled with the installed x64 Visual Studio environment, `/std:c++17 /O2 /W4 /WX /permissive- /utf-8`, the existing source and Windows SDK libraries, without building/installing a game DLL. Exact build scripts and result receipts are under ignored `local/camera-independent-gpu/`; CPU receipts are in `local/camera-independent-cpu/`. Existing host CMake targets already register these test files.

## Next ownership handoff

Fable owns production APIs and the runtime. Astra owns these three regression files and this audit document. Requested next artifact: total-motion API/first runtime commit. Then wire the same independent expectations into the actual new provider, test seed-only startup and source transitions, and run one focused final audit. No additional broad review gate or new user capture is requested. Game testing remains with Cort.

## Proposed runtime seam response (COMMS cc0c6c3)

Fable posted the first proposed API after this test increment: a total-mode flag, total RG32F/valid R32F outputs, separate raster facts, comparison/clear-based depth convention, reset delivery for no-pair frames, and a real camera-off switch. Approved that direction with the concrete corrections in COMMS. The response is implementation guidance, not a finding against code that has not yet been written.

The important unresolved boundaries are: (1) integer zero is not verified still, so bounded subpixel refinement must consider zero candidates before admission; (2) whole-field invalid motion does not reset NGX history, since the submitted xy field omits our provenance; (3) submitted first-image acquisition must advance independently of successful DLAA delivery; and (4) estimated total flow must not be reported as ENGINE ground truth. Preserve the residual path's existing semantics. Native DX11 first and DX12 second remain the agreed delivery order.

NVIDIA's [DLSS Programming Guide](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf), 31 March 2026, sections 3.13 and 3.15, prescribes InReset for major transitions and warns that BiasCurrentColorMask is unavailable for the latest models (legacy Preset F only). Therefore the existence of a mask parameter in the SDK does not make it a current-model solution for invalid/cut regions. The COMMS response proposes a GPU current-image transition with an asynchronously acknowledged reset epoch if the cut cannot be known at CPU evaluation time; that remains a suggested implementation requiring its own transition test, not verified runtime behavior. No synchronous diagnostic readback or repeated history reset in ordinary play is acceptable.

The 11-coefficient image/depth projective fit is deferred; it is not the current six-parameter camera correction and is not needed to expose and refine existing total flow. Fable retains production ownership. PR144 carries these independent tests; PR143 remains the earlier architecture audit.
