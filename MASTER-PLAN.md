# NeuralAliasAbundance: master plan

**Written by:** Claude, 2026-10-23, at Cort's direction. Incorporates Codex/Astra's recovery review (`docs/CLAUDE-HANDOFF.md`) and roadmap (`docs/INPUT-ACQUISITION-ROADMAP.md`).
**Audience:** Claude, Astra (Codex), Gemini Argon (independent gate auditor), and Cort.
**Status:** ACTIVE. This is the single source of truth for *what* gets built, *by whom*, and *in what order*.

> **Goal:** one universal DX12 injector that obtains or synthesizes everything DLSS needs (scene color, depth, motion vectors, jitter, camera) **without per-game code**. Universal coverage is the engineering goal and working design premise.
> Discover candidates, test conventions against geometric and photometric evidence, reconstruct or synthesize missing inputs, and save automatically verified profiles. Missing inputs create engineering tasks. Keep discovery active while the original renderer handles frames whose enhancement inputs are still being qualified. Measure progress with fixtures and a growing game/build/feature matrix. Protected-title access remains a separate integration/authorization requirement; no protection bypass is proposed here.

---

## 1. How we work (read this first)

| Rule | Detail |
|---|---|
| **Build mode now** | Cort's direction: **both agents build, nobody reviews yet.** Don't block on the other agent. Don't edit the other agent's files. |
| **Review gate sequence** | Starting at Gate 2: **Claude reviews every Astra PR; Astra reviews every Claude PR**. Authors resolve findings and run regressions, then **Gemini Argon audits the full gate snapshot and surrounding architecture**. Resolve/recheck findings, then run the scheduled Windows/GPU/game session and close the gate. Routine developer tests and CI continue throughout. |
| **Order** | Work your tasks **top to bottom**. Phases go from *already proven possible* → *hard*. Don't start a later phase's task while an earlier task of yours is open, unless it's blocked (say so in COMMS.md). |
| **Branches** | Claude → `claude/<task-id>-<topic>`, Astra → `astra/<task-id>-<topic>`. One branch per task. Commit and push when each task is done. Open a PR into `main` (it waits for the review gate). |
| **Coordination files on `main`** | `MASTER-PLAN.md`, `COMMS.md`, `COLLABORATION.md` and `HANDOFF.md` may be edited **directly on `main`** (pull/rebase first, small commits, never force-push). Everything else goes through a branch + PR. |
| **Talking to each other** | `COMMS.md` only. Append-only, newest at the bottom. Questions, blockers, done notices, interface requests and Argon handoff/audit status. Argon receives a commit-pinned review packet after cross-review; see COLLABORATION.md. |
| **Shared interfaces** | ABI 0.1 (`host_observe.h`) and `src/core/` stay frozen. Changes need a numbered proposal in COLLABORATION.md plus an ack from the other agent in COMMS.md. |
| **Never commit** | NVIDIA SDK headers/libs/DLLs, game files, captures, `local/`. |
| **Done means** | The task's "Done when" line is met on CI (Linux/Windows build) **or** clearly marked "needs Windows GPU" for the test session. No placeholder tests that pass by doing nothing. |

### File ownership

| Path | Owner |
|---|---|
| `CMakeLists.txt` (root), `build.ps1`, `.github/workflows/` | **Claude** (single editor) |
| `src/host_dx12/**`, `src/runtime/**`, `src/app/**`, `tools/**`, `scripts/deploy-*` | **Claude** |
| `tests/host/**` (GPU/host/proxy tests + stubs) | **Claude** |
| `src/providers/discovery/**`, `src/providers/classifier/**`, `src/providers/camera/**`, `profiles/**` | **Astra** |
| `tests/cpu/**` including `tests/cpu/CMakeLists.txt` (pulled in by root via `add_subdirectory`) | **Astra** |
| `tests/fixture/**` (ground-truth scene) | **Astra** |
| `src/core/**`, `host_observe.h` | **Frozen. Joint proposal required** |
| `docs/` | Each agent edits its own docs; shared docs by proposal |

---

## 2. The plan

Legend: **C** = Claude task, **A** = Astra task, **J** = joint task (the split is written inside it). Effort is rough engineering time.

### PHASE 1: Get the foundation compiling and tested *(most important; all known-possible)*

| ID | Owner | Task | Done when | Effort |
|---|---|---|---|---|
| **C1** | Claude | **Reconcile CMake.** Add targets for all 17 unbuilt `.cpp` files with correct deps and SDK feature gates (`NA_HAVE_NGX_SDK`, NVAPI, NVOF, PCL). Every SDK is optional: missing SDK = feature compiled out, build stays green. Add `add_subdirectory(tests/cpu)` and `tests/host`. | Configures and builds on Windows/MSVC with **no** NVIDIA SDKs present; feature gates logged. | 1–2 d |
| **C2** | Claude | **CI.** GitHub Actions `windows-latest`: configure + build Release + run CTest excluding label `gpu`. Linux job compiles the portable core + CPU tests with GCC. | Green CI on every push/PR. | 0.5 d |
| **A1** | Astra | **CPU regression tests** in `tests/cpu/`: frame_contract, temporal_jitter, frame_history, upload_cache, size_confirm, motion_arbiter (CPU tiers), host_observe framing/ABI static checks. Own `tests/cpu/CMakeLists.txt`. Label each test "reconstructed 2026-10" in a header comment. | Tests compile on GCC + MSVC and test real behavior, including invalid-input and loss cases. | 1–2 d |
| **C3** | Claude | **Host/GPU tests** in `tests/host/`: hooking, capture, gpu_timing, ngx_observe + `ngx_stub`, ngx_params, ngx_optimal, overlay, proxy, sl_proxy + `sl_stub`, `dx12_fixture`. Labeled `gpu` where needed. | Build on CI; marked "needs Windows GPU" for the test session. | 2–3 d |
| **C4** | Claude | **Fix R-001** (Astra's finding): GPU timestamp slots need a completion fence, no Present-count readiness, no reuse before completion; frequency taken from the executing queue. Regression test delays completion past 3 Presents. | Test in C3 covers it. | 0.5 d |
| **A2** | Astra | **Discovery contract reconstruction.** From ABI 0.1 + code comments, write `src/providers/discovery/CONTRACT.md`: what the consumer receives, what's known/ambiguous/lost (B-003…B-007). List the host data the classifier will need (feeds C7/C8). | Doc pushed; requirements posted in COMMS.md for Claude. | 1 d |
| **C5** | Claude | **Restore deploy scripts**: `scripts/generate-sl-proxy-def.ps1`, `scripts/deploy-sl-proxy.ps1` (manifest + SHA-256 + rollback). Also `tools/nr_replay.cpp` (local-only). | Scripts dry-run cleanly; documented. | 1 d |

**REVIEW GATE 1 → then Windows test session 1:** full `build.ps1` with GPU tests on Cort's RTX 5070 Ti, plus a TLOU re-check that the NeuralAlias menu and DLAA takeover still work.

### PHASE 2: Restore what already worked, and widen the easy coverage *(known-possible; big payoff)*

| ID | Owner | Task | Done when | Effort |
|---|---|---|---|---|
| **C6** | Claude | **Generic "feed DLSS" backend.** Our own NGX init + DLSS-SR/DLAA feature creation + evaluate from a filled `FrameContract`. Independent of any game. Proven on the DX12 fixture first. Everything in Phases 3–4 plugs into this. | Fixture frames → DLSS output on GPU; matches an expected-quality check. | 2–4 d |
| **A3** | Astra | **Ground-truth fixture v2** (`tests/fixture/`): camera pans, independently moving objects, animated geometry, disocclusions, transparency, HUD, camera cuts. Known depth/motion/jitter/camera for every pixel. This is the yardstick for every Phase 3 algorithm. | Fixture emits frames + truth files; CPU checker compares candidates against truth. | 2–4 d |
| **C7** | Claude | **FSR 2/3 and XeSS call interception.** Many DX12 games without DLSS still pass labeled color/depth/motion/jitter to FSR or XeSS. Intercept those calls and route the inputs into C6. Largest coverage gain for the least risk (approach proven by community tools; GPL code = reference only, no copying). | One FSR game and one XeSS game observed with valid inputs (test session). | 4–8 d |
| **A4** | Astra | **Upscaler detection report.** On injection, report which path a game offers: native DLSS / FSR / XeSS / none, with the evidence. Show it as an overlay status line through the existing menu API (`na::menu::Register`). | Overlay shows the detected route and why. | 1–2 d |

**REVIEW GATE 2:** Astra ↔ Claude cross-review → fixes/regressions → Argon independent audit → resolve/recheck findings → test session 2 (fixture DLSS plus FSR/XeSS games) → close gate.

### PHASE 3: Universal discovery, the brain *(harder; core of the goal)*

| ID | Owner | Task | Done when | Effort |
|---|---|---|---|---|
| **C8** | Claude | **Host capture H2:** PSO identity (hash at creation), per-pass render-target/SRV/UAV/DSV bindings, resource use order per command list, root constants. Delivered as new event kinds per A2's requirements (ABI minor bump, proposed first). | Events validated against the fixture's known passes. | 3–5 d |
| **C9** | Claude | **Host capture H3:** upload/constant-buffer content snapshots at submission time (copy, never synchronous readback), labeled `app-sync-unknown`. Needed for camera matrices and jitter. | Fixture camera matrices appear in snapshots. | 2–4 d |
| **A5** | Astra | **Signal classifier: depth + scene color (pre-HUD).** Score candidates by format, write order, clear values, value distribution, extent, pass position. Output a confidence and a reason for each. **Verify before trust** (e.g. depth must be consistent with the camera, color must contain no HUD). | ≥95% correct on fixture v2; never claims confidence on a wrong buffer. | 4–8 d |
| **A6** | Astra | **Camera + jitter finder.** Find projection and view matrices in H3 snapshots (structure tests: perspective form, near/far, frame-to-frame continuity), detect existing jitter, detect camera cuts. | Matches fixture truth; detects cuts. | 4–8 d |
| **A7** | Astra | **Motion-vector finder.** Identify existing MV buffers (2-channel float, ~0 when static, verified by warping the previous frame onto the current one). | Correct on fixture; no false positives. | 2–4 d |
| **A8** | Astra | **Auto-profiles.** Once a game's layout is confirmed, save it per exe hash in `profiles/` so the next launch is instant. Invalidate on mismatch. Hand-written adapters (TLOU) become overrides. | Second launch of a fixture "game" skips discovery. | 1–2 d |
| **A9** | Astra | **Anno 1800 diagnostics-only report:** which inputs were found, their confidence, and the unknowns. No reconstruction yet. | Report from a test session. | 1–2 d |

**REVIEW GATE 3:** cross-review → fixes/regressions → Argon audit → resolve/recheck findings → test session 3 (classifier output in Anno plus one more DLSS-less game) → close gate.

### PHASE 4: The hard parts *(save for last)*

| ID | Owner | Task | Done when | Effort |
|---|---|---|---|---|
| **C10** | Claude | **Live motion arbiter.** Wire Tier 2 (camera reprojection from A6's matrices + A5's depth) and Tier 3 (live optical flow, A52, already in code) into the frame contract, with per-pixel provenance. | Fixture motion error within budget; provenance visible. | 3–6 d |
| **J1** | Joint | **Jitter injection into games that never jittered.** Astra (A6) supplies *where* the projection lives and its convention. Claude writes the host-side injection (modify the matrix in the H3 upload path before submission) and keeps UI/shadow cameras untouched. | Fixture + Anno render with our jitter sequence; HUD unaffected. | 1–2 wk |
| **C11** | Claude | **Output replacement under the HUD.** Insert DLSS output at the pre-HUD stage found by A5 and composite the HUD back on top. Fallback to the Present-stage path when the stage is uncertain. | No HUD smearing in test session. | 3–6 d |
| **J2** | Joint | **MILESTONE: DLAA in a game that never had DLSS (Anno 1800)** at native resolution (no resolution change, which skips the hardest problem). | Cort sees DLAA working in Anno. | integration |
| **J3** | Joint | **True upscaling (render-resolution control)** for DLSS-less games: make the engine render smaller while UI/post stay at display size. Per engine family. Astra: find the resolution control points. Claude: host patching + restore on disable. | First engine family works at Quality/Performance. | weeks |
| **J4** | Joint | **Tier 1 motion vectors via DXIL patching** (research). Only after J2. | Research report + prototype. | weeks |

**REVIEW GATE 4:** cross-review → fixes/regressions → Argon audit → resolve/recheck findings → test session 4 → close gate.

### On hold (exists in code, not blocking the goal)
Frame Generation, Neural Rendering, Ray Reconstruction, Reflex/latency. Keep compiling; no new work until J2 ships.
**Before any of them is taken off hold, the "Required before un-holding FG/NR/motion" items in section 4 must be done.**

---

## 3. Environment (answered by Cort 2026-10-23)
- **NVIDIA SDKs:** all present on Cort's PC at `C:\Users\Cort\Documents\NeuralAliasAbundance-SDKs\`: `nvidia-dlss-sdk\` (NGX 310.9.16 headers + `nvsdk_ngx_s.lib`), `nvapi\`, `pclstats\`, `optical-flow\` (OF SDK 5.0.7 headers incl. `nvOpticalFlowD3D12.h`). DLSS runtime `nvngx_dlss.dll` 310.9.16 lives in `nvidia-dlss-sdk\lib\Windows_x86_64\rel\`. Everything project-related stays inside this one folder. **C1 makes these the default search paths** (overridable by cache variables); missing = feature compiled out.
- **Test games:** TLOU Part I (native DLSS) and Anno 1800 (none). Cort's DLSS games also expose FSR/XeSS, so they cover Phase 2 FSR/XeSS testing.
- **Windows host:** Cort's desktop (RTX 5070 Ti). Visual Studio and Git are **installed** (2026-10-23).

## 4. Build progress (2026-10-23, Astra)

- **A1:** implementation pushed as `acb29e9` on `astra/a1-cpu-regressions`.
  GCC Release: 7 CTests / 32 cases pass. MSVC and 6 Windows CPU host cases are
  unverified pending C2. Acceptance pending; no GPU is needed for these tests.
- **A2:** contract pushed as `c2dff24` on `astra/a2-discovery-contract` under the
  blocked-task exception while A1 awaits Windows CI. Host requirements
  A2-R01/R02/R03 are posted in COMMS for C7/C8/C9. No shared ABI change approved.
- **PR delivery:** Git push works, GitHub API returns Forbidden. Claude is asked
  in COMMS to create both task PRs. Reviews remain at Gate 1; no task code has
  been merged by Astra.

---

## 4. Audit notes: Gemini "Argon" architectural audit (2026-10-23)

Every claim was checked against the code (and, for API rules, Microsoft's documentation) before acting. Disposition per item:

| # | Claim | Verdict | Action |
|---|---|---|---|
| 1 | Presenter: `DO_NOT_WAIT` with V-Sync (`SyncInterval>0`) fails, dropping every generated frame | **Not a bug.** Microsoft's DXGI_PRESENT docs place no SyncInterval restriction on `DO_NOT_WAIT` (only `ALLOW_TEARING` requires SyncInterval 0 + the swap-chain flag + not exclusive fullscreen). The presenter forwards the game's own (valid) sync/flags unchanged. | Design note for FG (below): under V-Sync each extra present costs a refresh. |
| 2 | FG/NR/live motion copy any depth format into R32_FLOAT | **Confirmed.** Invalid copy for 24-bit (D24S8) and 16-bit (D16) depth. | **Fixed** (`claude/audit-hardening`): shared `na::depth` rule; FG/NR decline with a reason, motion falls back to motion-only. Conversion → **C12**. |
| 3 | FG feeds zero (singular) camera matrices outside TLOU | **Confirmed.** | **Fixed**: FG pauses without a validated camera. The proposed GPU "camera solver" with a synthesized 60° FOV / near 0.1 / far 1000 is **not adopted as a real camera**: field of view and near/far are not recoverable from depth + motion alone, and a guessed projection would be an estimated input mislabelled as engine truth. Kept as a research idea under J4/A6 (fit a clip→prev-clip reprojection from static pixels as a *labelled estimate* for validation/fallback). Removing the `TimingAttached` gate on `slSetConstants` camera capture → **C7b** (needs SL-version/layout evidence per game; `ValidCamera` invariants make it low-risk). |
| 4.1 | NR rejects depth/MV of different allocation sizes (DRS/padded) | **Confirmed.** | → **C13** (compare subrect coverage, not allocation size). |
| 4.2 | NR upsample shaders sample padded texture size, not the render subrect | **Confirmed by reading** (`GetDimensions` on the copy of the full plane). | → **C13** (pass render size + base in constants or crop the copy). |
| 4.3 | NR hardcodes `depth_inverted = 0` | **Confirmed.** | **Fixed**: read from DLSS create flags as FG does. |
| 4.4 | FG rejects display-resolution motion vectors (`MVLowRes == 0`) | **Confirmed.** | → **C13** (size/scale from `mv_low_res`, set MVecs subrect accordingly). |
| 5 | Arbiter / FlowExpand `SetDescriptorHeaps` would clobber a game list's heaps | **Not a live bug**: today both record only on NeuralAlias-owned lists. **Valid rule for later.** | **Rule for C10/C11**: host passes never record on game lists, or they restore the game's heaps; C8 records `SetDescriptorHeaps` (slot 28) per list anyway (needed for binding capture), which makes a `RestoreDescriptorHeaps` helper cheap. |
| 6.1 | `preset_profile` locked to `tlou-i.exe` | **Confirmed.** | → **C14**: use the running executable, but only as an explicit opt-in that records the user's original driver-profile value and restores it on disable (it is a persistent NVIDIA driver change that outlives the game session). |
| 6.2 | Universal mode re-query: `slFreeResources` for Streamline titles; return `FAIL_FeatureNotFound` once from Evaluate for direct-NGX titles | **Plausible, risky.** Faking a failure can make some engines disable DLSS for the session or crash. | → **J3 experiment**, behind a setting, per engine family, with evidence; never default. |
| 7.1 | Bindless (SM 6.6): root constants index `ResourceDescriptorHeap[]` | **Agreed.** Already in A2-R02 (root constants by value + descriptor versions). | C8 H2b must capture root constants **and** CBV/SRV writes into shader-visible heaps. |
| 7.2 | Persistent-mapped upload heaps sub-allocated by GPU VA | **Agreed** (matches A2-R03). | C9: map GPU-VA ranges → (resource, offset) from creation/`GetGPUVirtualAddress`; snapshot bound ranges at submission, labelled `app-sync-unknown`. Hooking `Map` alone is insufficient (one Map at startup); the VA→resource table is the key. |
| 7.3 | Depth is often cleared/overwritten late in the frame (weapons, 3D HUD) | **Agreed.** | C8/C11: depth lease taken at the end of the main geometry pass chosen by A5, not at Present. |

### New / amended tasks (Claude unless noted)
- **C7b** (Phase 2, with C7): generalize Streamline camera capture beyond TLOU (layout/version evidence first). **Done 2026-10-24** (`claude/c7b-sl-camera`): the TLOU-only gate is removed; SL 1.x vs 2.x Constants are told apart by the 2.x type GUID; FrameToken/ViewportHandle are read only after GUID checks; `ValidCamera` still gates every record. Also fixed a latent C5 bug: the proxy's typed wrappers used 1.x signatures and would have truncated 2.x pointer and 64-bit arguments (slInit sdkVersion, slEvaluateFeature FrameToken&); all wrappers now forward five full-width arguments.
- **C12** (required before un-holding FG/NR/motion): 24/16-bit depth → R32F conversion pass (SRV `R24_UNORM_X8_TYPELESS` / `R16_UNORM`, on NeuralAlias's own list at the submission boundary; requires the depth resource to allow SRVs, else decline).
- **C13** (required before un-holding FG/NR): subrect-correct inputs (#4.1, #4.2, #4.4) with fixture tests using padded/DRS-style resources.
- **C14** (FG phase): per-game NVIDIA driver FG preset, opt-in with restore.
- **FG design note**: under V-Sync, generated presents each wait a refresh; FG needs refresh ≥ (multiplier × game fps) or a VRR/tearing path (`ALLOW_TEARING` only with SyncInterval 0 on a tearing-capable, non-exclusive-fullscreen chain).


## 5. A3 build progress (2026-10-24, Astra)

A3 portable ground-truth generator and CPU candidate checker are implemented in
draft PR #12 (`astra/a3-ground-truth-fixture`, e57e830). CI run 40067259102 passes
Linux 8/8, Windows no-SDK 18/18, public-header 20/20 and deployment 32 checks.
Scene/truth output and ten analytical/adversarial checks are documented in
`tests/fixture/README.md` on the branch. C6 GPU upload/evaluation remains Claude's
integration task; Gate 2 peer review is pending. Next Astra task: A4.

## 6. A4 implementation status (2026-10-24, Astra)

Draft PR #13 at c063b95 implements/tests the portable evidence classifier and
Windows menu bridge. CI run 40067741880: Linux 8/8, Windows 19/19 and 21/21,
deployment 32 checks. **Not done:** host-owned runtime source registration and
menu-startup call await Claude (requested in COMMS). In-game visibility and real
FSR/XeSS evidence are not claimed; C7 supplies those producers. A4 remains open.
