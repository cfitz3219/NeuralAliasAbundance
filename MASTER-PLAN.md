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

### Gate 2 review fixes (2026-10-24, Astra)

Supersedes the earlier A4 wiring-pending status above. A3 #12 dd7e823 makes Python
optional for root configuration (fixture tests skip explicitly when unavailable).
A4 #13 61b74b6 completes host/menu wiring and consumes C7 at f2eeec41. Successful
creation, game requests with unverified outcomes, and injector DLSS are separate
evidence. CI passed: A3 Linux 8/8, Windows 18/18 and 20/20; A4 Linux 8/8, Windows
21/21 and 25/25, deployment 32 checks each. Claude's recheck and dependency fixes
remain pending; actual in-game acceptance and Argon audit are not complete.

### Gate 2 implementation takeover — 2026-10-24, Astra

At Cort's request, Astra completed Opus's outstanding Gate 2 code/test work in
[PR #17](https://github.com/cfitz3219/NeuralAliasAbundance/pull/17), integrating
#12–#16. Candidate **7b0339a** passes Linux 9/9, standard Windows 24/24,
SDK Windows 28/28 and 32 deployment checks. COMMS contains the exact head, CI
links and finding responses. C6/C7/C7b implementation ownership for this
completion task transferred to Astra; future phase ownership is unchanged.
Gate 2 remains open for independent delta/Argon review and RTX/runtime/game
qualification. No main runtime merge or game deployment has occurred.

### 2026-10-24 — PR #17 merged; Gate 2 validation remains open

Verified GitHub merged #17 into main as `89b93b627741ced40cc4dd2055d019be8454ef59`. Runtime, tests and build configuration match the CI-validated `0b2ee86` tree exactly. Implementation, automated validation and integration are complete. Remaining gate work is independent review of the takeover delta/unfinished Argon audit and the scheduled RTX/runtime/in-game qualification. No game deployment or gate closure is claimed. This supersedes earlier statements that the runtime changes were not yet merged.

## 2026-10-24 — Gate 2 CLOSED on Astra's side by Cort's direction

Cort explicitly directed: "close gate 2. On your end anyway". Astra's Gate 2 implementation and validation work is COMPLETE and CLOSED. This supersedes earlier statements that Astra's Gate 2 remains open. Evidence: integrated implementation in PR #17; fresh RTX Release build and 30/30 CTests; real NGX fixture output; Ghost FSR 3.1.4 and XeSS interception with successful DLSS replacement, zero observed fallbacks/rejections, and XeSS overlay INJECTOR confirmation. Live report and journal are in PR #19 at 88ddb9a; RTX report in PR #18.

Argon remains deferred, not represented as completed. Independent review/publication status and extended quality, DRS and device-loss coverage remain separately tracked follow-up work; they do not reopen or block Astra's Gate 2 closure under this user direction. No PR merge, new deployment, or additional runtime test is implied by this status change. Next Astra implementation task in the queue is Phase 3 A5, subject to its host-data dependencies.

## 2026-10-24 — GATE 2 CLOSED (both sides, by Cort's direction)

Cort: "finish this up and close gate 2!" Claude's side is complete, and with Astra's closure above, **Gate 2 is CLOSED**.
- **PR #20** (merged as 077adcc), Cort's RTX 5070 Ti, driver 32.0.16.2101: **31/31 CTests pass**.
- **Jitter, FSR:** official `amd_fidelityfx_dx12.dll` 1.0.1.41314 expects sample = centre − jitter on both axes, the same as DLSS (+3.3 dB over bilinear). The +1 mapping is confirmed.
- **Jitter, XeSS:** official `libxess.dll` 2.0.2.68, same result (+3.5 dB). The +1 mapping is confirmed.
- **Real DLSS:** +6.4 dB, sign −1, the same as session 3.
- **Device loss (G2-004), tested with real NGX and `RemoveDevice`:** the backend was keeping the removed device alive. D3D12 devices are one per adapter, so the game could never recover. Fixed: the Present and OnExecuted paths now call `dlss::ReleaseIfDeviceRemoved()`. Verified on the RTX: the game gets a live new device and DLSS restarts on it.
- **Game validation:** Astra's Ghost run (PR #18/#19) showed FSR 3.1.4 and XeSS replaced by DLSS.
- **Carried forward, not blocking:**
  - the deferred Argon audit of the takeover delta;
  - extended visual/DRS quality checks;
  - whether the overlay's own D3D12 objects also keep a removed device alive (not yet checked).

**Next:** Phase 3.

## 2026-10-24 — Phase 3 started by Cort

A5 is IN PROGRESS on astra/a5-signal-classifier, draft PR #21 (c7e53e9). First metadata ranking/abstention slice implemented; fresh Windows Release CPU/fixture CTest 11/11 passed. Full A5 content classification and >=95% fixture accuracy acceptance remain outstanding. Next: fixture candidate corpus, content/camera/HUD verification, C8 host ingestion after proposal acknowledgment and qualified async texture samples. Full A5 estimate remains 4–8 working days; no attendance for this first slice. Gate 2 stays closed and Argon deferred.

## 2026-10-24 — autonomous Phase3 checkpoint through A8

A5/A6/A7/A8 remain IN PROGRESS with independent CPU implementations in draft PRs21–24 (b53ad53/e635290/4341900/da205b6). Combined full Windows Release build and19/19 discovery CPU/fixture/menu CTests pass. Report: astra/phase3-integration/docs/PHASE3-DISCOVERY-CHECKPOINT.md at e0dd405. C8a standalone host test failure isolated to a default SRV on an untyped buffer; a private explicit-descriptor diagnostic passes RTX, fix sent to Claude. Live H2/H3/content acquisition, semantic qualification and A9 remain outstanding. No attendance needed for current development; game validation awaits qualified capture.

## 2026-10-24 — Phase3 host capture consumer checkpoint

Checked Claude5727047 and integrated latest checkpoints. Draft PR25 at46e92a6 adds bounded owning ABI0.1 structural decoder and actual transport callback coverage. Standalone11/11 tests; combined full Release at59f1463 and20/20 discovery CPU/fixture/menu tests pass. H2/H3, semantic provenance and texture acquisition remain pending; no Phase3 closure or game deployment. Detailed journal in PR25 src/providers/discovery/CAPTURE-WORKLOG.md.

## 2026-10-24 — Phase3 capture validity checkpoint

PR25 at1311ea0 now tracks authoritative status loss, inventory/capability boundaries, epoch changes and retroactive ambiguity. Standalone12/12; combined9cd8a76 full Release and21/21 discovery CPU/fixture/menu tests pass. Claude9655c85 integrated; committed RTX producer test still fails0xc0000409 and explicit-SRV private diagnostic still exposes capability session0 mismatch. Concrete corrections remain in COMMS. Phase3 remains in progress, no live deployment.

## 2026-10-24 — capture defects corrected, real consumer round trip passing

User-authorized fixes delivered in PR26 (1a81e1b, head0701da3): explicit buffer SRV, RAII stop, coherent capability session and startup/resync regressions. Combined22/22 selected tests now pass including RTX producer test. PR25 adds actual producer/decoder/validity round trip, passing RTX and forced WARP. Combined draft PR27 head946e019 runs full integration CI. Phase3 remains in progress for H2/H3, content acquisition and remaining host lifecycle/order findings. No game deployment or user attendance.

## 2026-10-24 — corrected capture integration CI green

Combined PR27 head946e019 CI run40090168495 passes all3 jobs. PR26 fixes and PR25 consumer heads also pass all3. Local broad suite40/40 non-GPU-labelled tests passes, zero skips, including RTX and explicit WARP consumer round trips. Both reproduced capture defects are corrected; remaining Phase3 acquisition/host review work stays open. Exact handoff and evidence in COMMS; no user attendance or game changes.

## 2026-10-24 — entry hooks integrated; ordering issue reproduced

Claude238f508 integrated with semantic descriptor-merge correction. Full Release and default40/40 tests pass at6fa529a. Separate pending overlap probe from PR25 a692599 fails3 assertions, reproducing Execute/Signal/Wait ambiguity-accounting gap. Default green tests exclude this explicitly opt-in pending regression. Exact producer fix request/ownership posted to Claude in COMMS. H2/H3 still unpublished; lifetime/order fixes remain in progress. No game deployment or user attendance.

## 2026-10-24 — queue ordering corrected; H2/H3 integrated

Queue defect fixed in PR29 focused3aa7745, head152cf47. Mandatory overlap and concurrent bookkeeping tests pass; all3 CI jobs pass run40092433784. Combined PR27 at7abf2bc integrates Claude H2 b736b6a / H3 29e8647, bounded decoder support and actual producer fixture wrappers. Local full Release and46/46 non-GPU-labelled CTests pass, zero skips. Latest combined CI pending. Prior pending-overlap and H2/H3-absent statuses are superseded for this integration branch.

Phase3 continues: H2 representational gaps, H3 underlying Unmap/copy lifetime and binding-provenance issues, and asynchronous texture evidence remain outstanding. C9-H3-REVIEW-1 in COMMS assigns producer fixes to Claude; exact queue patch supplied to avoid duplication. All APP_SYNC_UNKNOWN snapshots remain incomplete, not qualified camera input. No deployment or user attendance required for this checkpoint.

## 2026-10-24 — queue/H2/H3 checkpoint CI confirmed

PR27 head7abf2bc CI40092641546 passes all3 jobs. PR29 head152cf47 CI40092433784 passes all3; PR25 head1e1e467 CI40092363582 succeeds. Local combined46/46 remains the tested hardware/fixture evidence. Prior latest-CI-pending entries are superseded for these exact heads. Remaining Phase3 acquisition and producer lifetime/provenance work is unchanged; no merge/deployment.

## 2026-10-24 — consumer provenance tightened; C9 producer fixes still pending

PR27 head38ba5ce corrects incomplete bundle/list provenance and failed recording continuity; adds H3 delayed-order/epoch regressions. Full Release and46/46 local CTests pass, zero skips; CI40093056740 all3 jobs pass. Latest Claude c9-h3 remains29e8647 with lifetime/provenance findings unresolved. Producer work stays Claude-owned; consumer changes do not duplicate it. No deployment or Phase3 closure.

## 2026-10-24 — PR31 fixes integrated into Phase3

Claude fixes were published on claude/c8-c9-review-fixes (PR31), head fcbc57a, and are now integrated in PR27 at5ded6a0. Unmap ordering and binding dedup corrections are present; expanded root/viewport/scissor/copy payloads now have consumer bounds checks. Full Release and46/46 local non-GPU-labelled CTests pass, zero skips. CI pending. Remaining lifetime/coverage qualifications and async textures are coordinated in COMMS. Earlier unpublished-fix status is superseded; no deployment or Phase3 closure.

## 2026-10-24 — integrated PR31 CI confirmed

PR27 5ded6a0 all3 CI jobs pass40143583237, alongside local46/46 tests. Supersedes latest-CI-pending checkpoint. Remaining snapshot lifetime/coverage qualifications and async texture acquisition continue with Claude; no Phase3 closure or deployment.

## 2026-10-24 — repeated-binding provenance regression green

No newer Claude lifetime/coverage/async-texture commits at all-branch fetch. PR27 b55de74 adds default snapshot_bindings test: RTX8 records/8 associations across same-address root/table bindings in two lists, with payload ownership verified after overwrite/Unmap. Full Release succeeds; targeted test passes. All3 CI jobs pass40144391920. Existing47-test default suite now includes this regression; no claim of a new local full-suite run (previous46/46 remains recorded separately). Remaining producer lifetime/coverage and async acquisition stay Claude-owned; no deployment.

## 2026-10-24 — C9b texture samples reviewed and experimentally integrated

All Claude branches checked. New67b7744 is integrated in PR27 a171f40, preserving existing fixes/tests. Full Release and48/48 local non-GPU-labelled CTests pass, zero skips, including correct RTX color/depth samples. Review verdict: changes required before live consumer use. C9B-R1–R6 cover repeated cancellation, source/failure ownership, queue boundary ordering, state coverage, expiring borrowed pointers and format/subresource validation. Full report docs/C9B-REVIEW.md on integration branch; exact corrections coordinated in COMMS. No production sample-request caller enabled, no deployment. CI pending at this entry.

## 2026-10-24 — C9b combined CI confirmed

PR27 a171f40 all3 CI jobs pass40146126274. Local48/48 passes. Supersedes pending CI above, not C9B-R1–R6 live-use blockers. No deployment; producer correction ownership remains Claude.

## 2026-10-24 — C9b fixes integrated and rechecked

All branches checked; PR33 head688c0ec integrated into PR27 at6cd9d11. Full Release and48/48 local tests pass, zero skips. Original R1 repeated cancellation and R5 borrowed pointer corrected; retention/quarantine, queue locking, evidence and shape checks present. C9B-REVIEW-2 retains four changes-required findings: worker proliferation/static lifetime, raw source acquisition, scope validation deferred to maintenance, typeless depth partial-copy gate. Producer fixes remain Claude-owned; no live consumer or deployment. Latest CI pending.

## 2026-10-24 — C9b fix integration CI confirmed

PR27 6cd9d11 all3 CI jobs pass40147665279, alongside local48/48. Latest-CI-pending superseded. Four C9B-REVIEW-2 findings remain; no live enablement or deployment.

## 2026-10-24 — PR34 verified; pending worker regression reproduced

PR27 a5b11f2 integrates Claude629ad9b. Full Release/default48/48 local tests and all3 CI jobs40149217862 pass. Separate na_sample_worker_wakeup diagnostic FAILS on RTX: warmup timeout works; second20ms request after idle remains ARMED after~1s. Default CI explicitly excludes this known failure (NA_ENABLE_PENDING_SAMPLE_TESTS opt-in). Acquisition, immediate scope, typeless-depth and singleton guard reviewed/tested; remaining worker wakeup and bounded-teardown proof belong to Claude. No live consumer/deployment. Exact repro/report in COMMS and integration docs/C9B-REVIEW.md.

## 2026-10-24 — PR35 integrated; wakeup regression now passes

PR27 head8ee89dc integrates Claude47b1208 after629ad9b. Full Release and all49/49 local CTests pass, zero skips, including RTX sample fixtures and mandatory na.host.sample_worker_wakeup. The previously failing/excluded regression is corrected; its pending gate is removed. Pre-submit acquisition, immediate scope checks, typeless-depth restrictions, singleton worker, notification and normal thread-handle teardown paths reviewed/tested. Combined CI40149547458 pending. Remaining producer failure path: DuplicateHandle failure still detaches without shutdown ownership, reported to Claude; deterministic queue interleaving, failed-Signal quarantine and plane rejection remain test qualifications. No live sampling consumer or game deployment. Earlier PR34 known-failure/exclusion entries are historical and superseded for this head.

## 2026-10-24 — PR36 integrated; four findings addressed in reviewed paths

PR27 f250a36 integrates Claude c15881d. The thread retains its original native handle; no DuplicateHandle fallback or detach remains. Injected thread-start failure refuses the request with zero live samples/workers, then retry succeeds. Full Release passes. Expanded RTX run:54 registered,53 passed,1 skipped,0 failed; includes all49 non-GPU-labelled tests plus real DLSS/device-loss. FSR/XeSS jitter fixture skipped for missing configured runtime DLLs (none found in local SDK/local artifact directories); not counted as passed. Raw logs local/phase3-integration/build-review4.log and ctest-review4-full.log. Four original findings and follow-up wakeup/handle gap corrected in inspected paths. Deterministic queue interleaving, failed-Signal quarantine and plane rejection remain test qualifications with Claude. No live sampling consumer/game deployment. CI40149901349 pending; predecessor8ee89dc all3 jobs passed. This supersedes the previous open handle-duplication finding.

## 2026-10-24 — Consumer cross-review R2–R6 corrected

PR27 e631083 integrates PR34–36 and fixes ASTRA-P3-R2–R6: robust motion coverage/inlier/percentile policy with textured-pan regressions, independent matrix-kind caps, reflected views/handedness discontinuity, partial sky depth evidence, and bundle/indirect incomplete capture. Full Release and49/49 non-GPU-labelled local tests pass, zero skips. Existing camera/signal/motion corpora pass; motion160 hypotheses retains30 correct,90 wrong-sign/scale contradictions,40 sparse unavailable. Latest CI pending. Preceding f250a36 all3 CI jobs40149901349 passed, plus expanded RTX53 pass/1 missing-upscaler-runtime skip/0fail.

R1 production evidence acquisition remains open (temporal depth/camera, H2 HUD boundary, visibility); A5 acceptance/A9 remain outstanding. Motion thresholds are initial evidence policy, not live acceptance calibration. Producer adversarial interleaving/Signal/plane qualification remains Claude-owned. No live sampling enablement or game deployment. Detailed journal on integration in src/providers/discovery/CAPTURE-WORKLOG.md.

## 2026-10-24 — Consumer corrections CI confirmed

PR27 e631083 passes all3 CI jobs40150308066 (Linux, Windows, Windows with public NVIDIA SDK headers), alongside full Release/local49/49 zero skips. Merge-tree is clean against current main; all-branch fetch found no later Claude producer revision than c15881d. Supersedes latest-CI-pending above. Cross-review requested for R2–R6 corrections; R1 production evidence/A5 acceptance/A9 and Claude's adversarial producer qualifications remain open. No live consumer enablement or deployment; no user attendance required for this checkpoint.

## 2026-10-24 — Production acquisition/depth checkpoint

PR27 e55c191 includes Claude PR37 ab625be and new bounded worker-side SampleAcquisition/HostSampleApi, exact actual queue/ticket camera association and temporal depth corroboration. RTX fixture acquires two completed32x16 D32 samples (3968 bytes each), validates496 pixels with16 border exclusions. CPU ownership/expiry/cancellation/shape/row-layout and temporal scope/reset/reverse-Z tests pass. Fixture-v2 temporal corpus:30 valid pairs consistent,30 corrupted contradicted,6 camera-cut unavailable; no world-position/visibility oracle supplied. Sparse finite MV sentinel follow-up fixed with explicit outlier accounting. Full Release and53/53 non-GPU-labelled local CTests pass, zero skips. Latest CI pending.

Claude accepted prior R2–R6. PR37 Signal-failure quarantine and plane rejection tests pass; interleaving probe is still timing-assisted, refinement requested. Claude owns off-by-default read-only host activation/worker, Astra owns evidence consumers/A5/A9. R1 still needs qualified live camera/frame association, semantic corroboration/visibility and preserved pre-HUD color; submission-end bytes cannot recover overwritten scene contents. A5 >=95% end-to-end fixture acceptance and A9 actual Anno report remain open; temporal agreement is not their substitute. No game deployment, live activation or effect promotion. Journal and interface: integration src/providers/discovery/{CAPTURE-WORKLOG.md,PRODUCTION-EVIDENCE.md}.

## 2026-10-24 — Production evidence CI confirmed

PR27 e55c191 passes all3 CI jobs40154026855 (Linux, Windows, Windows SDK), alongside full Release/local53/53 zero skips and the real RTX acquisition test. Merge-tree clean against current main; no later Claude branch beyond ab625be at final fetch. Supersedes CI-pending above. New acquisition modules and sentinel follow-up await peer review; R1 semantic/live association/pre-HUD work, A5 acceptance and A9 session report remain open. Host activation stays Claude-owned/off by default; queue handshake refinement requested. No live activation or deployment.

## 2026-10-24 — Anno A9 delivered; final implementation review at91747c4

PR27 now includes production_checks APIs,264/264 combined fixture acceptance,read-only live adapter,actual Anno report and a reproduced transport exit correction. Full Release/58 of58 local non-GPU-labelled tests pass,zero skips/failures; latest CI pending. A9 rendered capture:139414 submissions,21664 live resource records,3315759 snapshot events,0 malformed/drops,0 confirmed inputs. Follow-up inventory records/8 arm timeouts are documented; no completed samples or qualified frame/camera/pre-HUD evidence. Both deployment manifests verify rollback. Baseline DX12 black-screen fixed by preserving unreadable OneDrive shader cache and regenerating it; game executable/settings preserved.

Windows Event1000 exposed0xc0000409 on DLL exit. Minimal EXE regression reproduced before patch,passes after; active-DLL ExitProcess regression passes. Post-fix in-game shutdown and early follow-up termination remain unverified. This is reviewable code/evidence,NOT Gate3 closure or qualified Anno compatibility. docs/PHASE3-REVIEW-STATUS.md and docs/A9-ANNO-DIAGNOSTICS.md supersede earlier snapshot-only/unfinished-report handoffs. New lifecycle delta awaits Claude review. Exact source head91747c480898a88bdc994dcef3f350d812a3f4b2; regenerate review packets for it. Do not treat original e55 packets as current. No diagnostic installation remains.

## 2026-10-24 — Final Anno review-head CI confirmed

PR27 head91747c480898a88bdc994dcef3f350d812a3f4b2 passes all3 CI jobs40156916140 (Linux,Windows,Windows with public NVIDIA SDK headers). Full Release/local58 of58 passes and merge-tree is clean against main. Supersedes latest-CI-pending above; compatibility findings and review requirements unchanged. Exact source archive and e55c191 delta are at local/gate3-review-91747c4/. Archive SHA256 DE1B474F3F076D0161F0FAEF9F78C3B5414EAB63B9C6F1CC2EED87B508860C08. Both game deployment files absent at final verification; no live diagnostic installation.


## 2026-10-24 — Final planned Anno verification completed; installation restored

PR27 source9e1b140, report head6a674c3. Fixed/proved nonzero RTV/DSV descriptor resolution and actual driver Wait re-entry startup crash; normal in-game exit verified twice. Final planned single launch PID37488 rendered coastal gameplay and read-only overlay, logged16,419 transport Presents,12 completed texture samples and captured-buffer matrix scans. Normal Save and Exit returned0 at04:03:07Z; rollback verified.61/61 local tests and all3 source CI jobs40158744541 pass. Full A9 metrics, qualifications and hashes are appended in integration docs/A9-ANNO-DIAGNOSTICS.md; local evidence gate3-anno-final-verification. No further Anno launch scheduled for these checks.

Not Gate3 closure: camera execution-time provenance, packed-depth acquisition, preserved pre-HUD lineage, motion corroboration and complete capture remain unavailable. Latest capture has4,592 detail drops and conservative inventory-invalid status from explicitly unobserved swapchain creation provenance;0 malformed/foundational drops. Do not promote samples/Presents into qualified DLSS inputs. Review the current code, not917/e55 packets. Claude branch stillab625be at all-branch fetch; no later camera/provenance fixes found.

## 2026-10-24 — Astra implementation/report handoff packaged at3196180

Owned A5–A9 implementation and reports are mapped to the agreed acceptance criteria in integration docs/PHASE3-ASTRA-COMPLETION.md. Final code7bc3f1e connects live diagnostic texture records to the actual A5 classifier reasons/acquisition tasks instead of a generic placeholder; inventory-only records remain unavailable. Release runtime/adapter build and diagnostic_adapter, production_checks and 264-case production_classifier tests pass. Report-only change was not redeployed. Prior game-tested9e1b140 has61/61 local tests and all3 CI jobs passing; newest-head CI not yet reported.

Exact tracked source head3196180d0edb80039041106ec06bff1a3228cc3c is pushed in PR27. Review package local/gate3-review-3196180 contains source ZIP, delta from91747c4, acceptance map and SHA256 manifest. Source ZIP SHA256 E8579A86B2094DE7987F2022F0483E7D29B277173FE87CA378394BF75330E215. No SDK/game/capture binaries included. Installation remains restored; no further launch needed for this reporting delta.

Claude: please review current PR27, including442d7d5 descriptor resolution,30b4256 reentry/lifetime,9e1b140 Present/scan fixes and7bc3f1e reporting. This supersedes old review packets, not historical compatibility qualifications. No approval inferred. Execution-time camera/packed-depth acquisition remains your requested lane; no duplicate edits planned. Gate3 remains open for peer review, audit/fixes and the planned additional-game check. Anno camera, depth, pre-HUD and motion qualification remain unavailable; A9 reports these unknowns rather than claiming working DLSS. No user attendance is required for this handoff.

## 2026-10-24 — REVIEW-2 fixes tested; complete Argon package awaiting final peer check

Integrated Claude PR38 through77f49ef.90f165b addresses R9–R13 with regressions;9085418 fixes the additional nested-Signal provenance gap found during PR38 review. Both ordering regressions fail against77f49ef (incorrect READY), pass fixed. Full Release61/61 tests and all3 CI jobs40170054616 pass on9085418. Final documentation head6cfa87caa1feff3f0ee83dedcf21234b080b25f1 is pushed in PR27. No game deployment/restart; rollback still verified.

Complete package local/gate3-argon-6cfa87c.zip includes all320 tracked files, full base delta/history, included PR heads21–38, both peer review records, coordination snapshots, reproducible commands, positive/negative test evidence and per-file hashes. Package SHA256246475C802D8B81217B2A52A9AA5A4244FA53F725D41A1771F16ACD38805E7FC. Every packaged/source hash verified; no compiled/proprietary binaries included. Base843d0ce15e2b3c7dcf70d46f81ccd67f11a015cc. Detailed dispositions: integration docs/PHASE3-REVIEW-2-DISPOSITIONS.md.

Formal Argon status: NOT READY solely pending Claude's final fix/inventory check requested above, not an inferred approval. All currently identified implementation findings have corrections/tests; independent audit has not been delivered or run. Claude: please check90f165b/9085418 and the package includes your producer work/qualifications; publish acceptance or concrete remaining findings in COMMS. That reply permits updating the packet/status for Argon without user attendance. Existing Anno unknowns and later game gate requirements remain explicitly documented, not treated as passing DLSS inputs.

## 2026-10-25 — Astra accepts PR39; ready for Argon

Reviewed/accepted52cde25's per-queue ordering window: no operation may begin on the sampled queue from evidence submission to copy submission. Checks before servicing and after forwarding reject pre-copy/overlapping interference; later FIFO-ordered operations cannot change the copy and retain READY. This correctly replaces my overly global9085418 invalidation. Cross-queue pre-copy and post-copy Signal regressions pass. Full local Release61/61, zero failures/skips, and all3 CI jobs40170612165 pass at52cde258dc1ab6d1dc112996206af98f8d16bb6e. Claude already accepted90f165b R9–R13 and conditionally cleared handoff upon this acceptance; condition now met. P3 extra view-family formats remain explicitly unsupported plans, not silent decoding, non-blocking for the declared resource formats.

Final combined candidate ed6b8b256dd5050b5f9302a2237765e1e3f80606 (documentation after tested52cde25) pushed in PR27. Status READY FOR ARGON; no audit delivered/performed or Gate3 closure inferred. No deployment/restart. Please regenerate any truncation-safe packets at this exact head, superseding6cfa87c. I am finalizing the full tracked source/delta/review/evidence bundle with hashes; no further source changes planned absent audit findings. Your PR38 andPR39 work and qualifications are included.
