# HANDOFF

Short version (2026-11-18). For the current state of the work (owners, active branch and PRs, what is on Cort's PC, open items, rules), read the **"Current state" section at the top of COMMS.md**. This file keeps only the latest handoff notes.

The full previous journal (1,177 lines, 213 KB, 2026-09 to 2026-11-17: milestone 1, gates, Anno, Sniper, PBZ, Ghost, WWII, BF4, HOI4, Rome II, materials, camera-independent DLAA) is kept word for word in [`docs/comms-archive/HANDOFF-through-2026-11-18.md`](docs/comms-archive/HANDOFF-through-2026-11-18.md).

**Keeping it short:** add a few lines per milestone (what is ready, where, what it waits on). When this file passes ~20 KB, move the older notes verbatim to the end of a file in `docs/comms-archive/`.

## Latest handoff notes (verbatim)

### 2026-11-17 — GitHub backlog and main consolidation complete

Reviewed all 39 original open PRs. Merged #133, #132, #134, #135 and #144 into Route B, then merged #158 to main as 82fe548 after all four GitHub jobs passed. That brings the consolidated DX11/DX12 source and the latest GameCheck rules into main. GameCheck passed 38 tests locally. Closed 24 obsolete, duplicate, or carried-forward PRs after verifying their disposition. The remaining ten are #83, #88–#94, #142 and #145. Earlier branches are retained. Fable's active Resistance build and follow-up are still tracked by #145. See docs/PR-BACKLOG-20261117.md for the evidence and decision on each original PR.

### 2026-11-17 — Referee stage 1 patched

PR #159 at a867810 targets Fable's active branch. Exact-frame and swap-chain matching, independent fences, bounded recording lifetime, actual NGX snapshots, separate submission coverage, lower DX11 copy load, and off-thread scoring are implemented. Release host and five focused checks passed, plus WARP. COMMS has the integration notes. The referee is opt-in pending real-game calibration. No package or installed game changed.

PR #159 merged to the active branch at 45441f2. The merged tree is identical to the tested tree. Saved-capture calibration and wiring the verdicts to the route selector are still separate work.

PR #161 merged at 3536dbba. It connects the referee to a read-only route preview and replays saved Resistance captures through the same policy. It does not give the referee control of the live route. COMMS and the active WORKLOG record the samples, controlled bypass and limits of the evidence.

## 2026-11-17 Pacific — Native DX11 jitter-locked lighting handoff

Astra's native DX11 implementation is a561d9e (PR #162 into Fable's claude/project-thread-axt148). Fable retains the DX12 implementation and combined integration. The Release host built, hardware and WARP passed the depth-slab fixture, and the debug-wrapper fallback was verified. Original depth tests and DLAA inputs are intact. COMMS records the rendering condition, state-handling decision and game limits. Source handoff only. No installation, package or live game run.

## 2026-11-17 Pacific — Cross-review of depth correction

Cort requested a reciprocal review. Astra asked Fable in COMMS to review DX11 PR #162 at a561d9e (12/12 Release tests). Astra reviewed Fable's DX12 2db2250. The original fixture and shader checks passed on the RTX, but narrow hardware/WARP probes reproduced premature twin activation, descriptor semantics changes, residual viewport-expansion error, and injected query/work duplication. COMMS carries the measurements, DX12 corrections and local reproduction path. Fable owns DX12 fixes. No deployment or gameplay test.


## 2026-11-17 Pacific — Adaptive referee audit complete on isolated draft

Astra's draft PR #163 (5a84308) targets Fable's active code line. It improves DX12 placement discovery when the final scene writer changes and lets the shared referee relocate its picture comparisons, use isolated or padded evaluations and distinguish intermittent delivery. Thirteen automated regressions passed, including native DX11 and DX12. The referee ran on the RTX 5070 Ti and WARP. Two saved Resistance runs gave 60/60 output-favoring screens, and a controlled alternating replay flagged mixed delivery. The draft's WORKLOG records limits and reproduction evidence. Cort's false-motion hold still applies to merging and packaging.

## 2026-11-17 Pacific — Native DX12 screen-check handoff

Sol's draft PR #164 (b71fa1b, base 0cb1fb46) reuses the scale bridge's screen probe to verify native DLAA placement. RTX 5070 Ti rejected the decoy and confirmed the real reader, with 29 consecutive presented frames carrying the test evaluator's output through the game's own post-processing. The Release host and all 16 focused checks passed. The new fixture also passed on WARP. Fable owns integration and packaging. Shadow of War's DX11 route is outside the new path.

## 2026-11-17 Pacific — Native DX11 screen-check handoff

PR #165 (89ae5f5 on 8e2eb95b) applies the shared screen check to the native DX11 provider. RTX 5070 Ti and WARP each rejected the decoy, confirmed the visible reader and matched presented pixels on 53 consecutive native-size frames. Eleven selected local cases passed, including two separate native DX11 tests using real NGX. Fable owns integration and packaging. Shadow of War still needs its trial.

## 2026-11-18 Pacific — Shadow of War DX11 Referee continuity

PR 166 (07acf67) targets the active 78b539df integration line. The native DX11 Referee now preserves ordinary DLAA delivery and requires a candidate to stay present throughout screen proof. A reader that later alternates between delivery and misses loses control. Ten focused tests passed. RTX and WARP pixel checks passed for ordinary delivery, interrupted proof, discovery of the visible reader and restoration after alternating misses. No live game run or installer. The separate overhead from camera discovery while DLAA is paused is documented in the branch's WORKLOG.

## 2026-11-18 Pacific — Shadow of War final-image monitoring and camera cost

PR 166 ends at c46e5b9 on the active native DX11 base. Referee checks the exact presented frame after approval, aligns the scene and display grids, and retires only the matching reader after repeated clear input votes. Pausing DLAA stops camera acquisition and DLAA-only sampler bias. Optional camera search backs off during successful measured-motion delivery and resumes automatically. Release and 13 focused automated tests passed including native DX12 screen coverage and three real NVIDIA DX11 cases. The classifier passed separately on WARP. COMMS and the branch WORKLOG preserve receipts. No new installer or matched in-game timing on this build.

## 2026-11-18 Pacific — Native DX11 changing draw order

PR 167 (b213ff5) on 37247cff reuses DX12's pass-role profile and verifies independent paths for recurring DX11 draw orders sharing the same scene inputs. Release and all 18 focused checks passed. Real NVIDIA DLAA on the RTX 5070 Ti matched 32/32 alternating presented frames, qualified a third order after rejecting a decoy, and recovered known orders immediately. COMMS records coverage and Fable's assignments for witness samples, lighting/depth, bow misses, and integration. Cort owns the game trial.

## 2026-11-18 Pacific — Combined Shadow of War stability trial

Astra completed the menu and aiming placement work, native Present witness and depth-read continuity in PR 167 through 5ebf86a. The combined RTX and CPU suite passed 21/21. The native production fixture recorded eight screen samples and matched real DLAA on all 32 alternating screen frames. The reversible Shadow of War package is staged in Documents\NeuralAlias-ShadowOfWar-Stability-5ebf86a. Install, update, results and rollback passed in an isolated fixture. Cort retains the live checks for lights, bow aiming and menu transitions. See COMMS for the remaining uncertainty in the old run.

## 2026-11-18 Pacific - Native DX11 Referee continuity

PID 27836 from 5ebf86a identified two complete no-submission windows during adaptive search. PR 168 repairs the takeover, passes 21/21 focused tests including real RTX displayed pixels, and is staged in Documents/NeuralAlias-ShadowOfWar-Referee-2e062ef. The active merge 650b854e and tested 2e062ef have identical trees. Bow aim visibly warps the downstream image and hides the text while registered center detail still favors DLAA. A separate rock-edge variation first becomes large after DLAA; the exact pass remains unidentified. Fable's independent pixel measurements and Astra's comparison are recorded in COMMS. Cort owns the live game test.

## 2026-11-19 UTC - Referee screen registration and K/M split

Cort's 2e062ef run, PID 33800, produced six captures. In PR 169, automatic native-pixel registration recognizes displayed DLAA in 173/180 saved evaluations, previously 5/180 at the same probes. The closed-frame eligibility change excludes deliberate runtime pauses. Eleven focused automated checks passed. The remaining local reflection is color-related in held-out comparisons on both presets. Astra owns the later lighting path. Fable accepted the K/M input and motion lane. No new install or game test. See the report on PR 169 and the 08:13Z COMMS entry.

## 2026-11-19 UTC — Native DX11 rotating lighting role

PR 169 at e1d2db6 keeps the scene history while selecting the current screen-proven lit buffer. Nineteen focused tests passed, including two RTX reflection comparisons with actual NVIDIA DLAA, an offscreen decoy, source invalidation and an intentionally extinguished light. Package and rollback are in Documents/NeuralAlias-ShadowOfWar-LitScene-e1d2db6. Cort owns game testing. Fable's K/M and motion investigation is separate. Details in COMMS and docs/REFEREE-RENDER-LAYERS-20261119.md on the code branch.

## 2026-11-19 Pacific — Referee proof and recovery audit

PR 170 at 7d1dc19 follows PR 169. It hardens DX12 proof against periodic scene changes and DX11 recovery against transient loss, preserves motion diagnostics across verified lighting swaps, and separates split composition from whole-frame rollback. Twenty-one integration checks passed, followed by five final GPU and screen fixture passes. The installer, Results and rollback are staged in Documents/NeuralAlias-ShadowOfWar-RefereeRecovery-7d1dc19. Live confirmation is Cort's. Fable retains the separate replay and K/M lane. See the audit on PR 170 for the complete failure inventory, references and open cases.

## 2026-11-19 Pacific — Accepted presentation and regional flicker tracing

Stacked PR 171 at 8bb03bb extends the native Referee with accepted Present receipts and regional stage attribution. Fifteen focused regressions passed. The final RTX fixture verified all three stages, centered padding and rejection of mismatched or failed receipts. Installer, Results and rollback are staged in Documents/NeuralAlias-ShadowOfWar-RefereeTrace-8bb03bb and passed in an isolated fixture. See docs/REFEREE-PRESENTATION-TRACE-20261119.md on the code branch. Cort owns game validation. Fable owns the separately demonstrated motion-fit fault.

### 2026-11-19 Pacific — Sol — Controlled Referee replacement

PR #172 at 02ed22b compares ordinary Native11 delivery with a screen-proven alternate and then restores the ordinary route for an independent recheck. It keeps the alternate only for verified improvement with visible delivery. Real NVIDIA DLAA and rollback decisions were checked on RTX. Seventeen automated checks passed. The reversible Shadow of War test package is Documents/NeuralAlias-ShadowOfWar-RefereeTrial-02ed22b. Install, Results and rollback passed an isolated fixture. Cort retains game testing. Fable retains the far-motion work.

### 2026-11-19 Pacific — Astra — Evidence audit of the Referee replacement

Commit 709f3c1 on PR #172 corrects false delivery votes, stale comparison evidence and failure to revisit a previous decision. The verified Shadow of War package is Documents/NeuralAlias-ShadowOfWar-RefereeTrial-709f3c1. Sol's 02ed22b remains available. Seventeen selected checks passed, including real RTX DLAA, plus archived-capture replay and isolated rollback. Cort owns the live reflection test. Details are in the latest COMMS handover and the PR's presentation trace audit.

## 2026-11-19 — Astra — Referee recovery and outside review

PR 172 at 4b211e8 contains the cross-game native DX11/DX12 stage-to-tool recovery. The RTX worker now returns the origin of a local flash and the controls actually used by the two accepted frames. The real DX11 rotation fixture rejected placement and RCAS in sequence while continuing DLAA on the next 28/28 frames. A separate controlled GPU input fixture rejected its first change, then measured 0.246/0.000/0.246 stops and kept the second. Details in docs/REFEREE-PRESENTATION-TRACE-20261119.md and WORKLOG.md. These are fixture results. Cort owns game testing.

Neon's eight proposals and Fable's response were independently checked in docs/REFEREE-NEON-REVIEW-20261119.md. COMMS has the saved-frame margin comparison and the proposed regional and motion checks. This candidate also includes Fable's PR 145 motion changes through b9f8342d. The RTX uncovered a 28-sample disagreement about negative nearness in the shared test field. Fable owns that correction and has the reproducer in COMMS 336b7ff. Hold the installer until the combined checks pass.

### 2026-11-19 — Referee lighting and mixed-screen handoff

PR 172 at 0e6f79b adds the patch-local gain measurement, an inconclusive class, and matching regional witnesses on accepted screen receipts. DX11's existing reversible lighting-placement comparison can test a localized bypass and independently restore the original for confirmation. The release build, seven focused tests on CPU/RTX, both Shadow of War capture replays, and the Results-reader regression completed. The two captures contain 430 light-compatible, 1,048 uncertain and 5,167 materially changed still-point observations under the new rule. No new game run or installer deployment. Fable owns the separate PR 145 motion fitter disagreement documented above.

### 2026-11-19 — Added-DLAA overlay controls

PR 173 at 71c731bc removes the manual moving-object flow switch and slider and fixes the production tuning at 50%. Keep Steady Distant Lighting and draw replay available for separate comparisons. This is a tested source and release-build candidate atop PR 172. Existing game packages remain at their prior builds.

### 2026-11-19 — Clean CPU rebuild check

PR 173 source 71c731bc rebuilt in build-cpu-verified-71c731bc. Runtime DLL and screen-motion test section data matched their earlier build after excluding linker debug records. All 31 common generated headers matched. The known screen-motion disagreement reproduced twice, while 11 other checks passed. During the build Windows logged one corrected processor-core parity exception on APIC ID 32. See COMMS and the branch WORKLOG for the DISM overlap, hardware record and scope. No candidate installer was published.

### 2026-11-19 — Combined Shadow of War candidate

Use Documents/NeuralAlias-ShadowOfWar-Combined-55527b0/02-INSTALL-ShadowOfWar.cmd. This combines the two Referee changes from PR 172 with the depth-bound correction and 50% optical-flow setting on PR 173. The 28-sample disagreement is fixed and verified twice on the RTX. Fourteen other automated regressions and the actual packaged DLSS runtime placement trial passed. Package and reversible upgrade tested without touching the real installation. Cort retains the live reflection check and can run 03-RESULTS-ShadowOfWar.cmd afterward.

### 2026-11-19 — DX12 Referee placement comparison

PR 174 at 7bb6b815 adds the qualified separate-list alternative and screen comparison to DX12, on top of PR 173. It checks the actual qualified first reader and scopes the trial to the scene and evaluation settings. A failed comparison restores normal selection. The first missing candidate can cost one DLAA frame if the incumbent inline list has already closed. Results parser regression passed. The Windows CI check and eventual game validation are separate. Shadow of War was open, so local compilation, GPU tests and installation were deferred.

Final branch head 9912d332 adds reader-role qualification before nomination and rules out earlier reads when a later list writes the selected scene again. GitHub Actions run 44659158634 passed all four jobs, including the controlled DX12 screen comparison on both Windows x64 configurations. Linux 63, Windows without the SDK 155, Windows with the public SDK 204 passed and one skipped NR runtime probe. See PR 174 for the code and COMMS for the exact pixel witnesses.

### 2026-11-19 — Shadow of War reflection-reader package

Documents/NeuralAlias-ShadowOfWar-Reflection-07a2004/02-INSTALL-ShadowOfWar.cmd packages PR 175 with the previous motion and depth-bound correction. The RTX fixture proved that real DLAA reaches the screen after downstream depth reuse and keeps history across both lighting surfaces. The automatic comparison reduced measured variation from 8.9974 to 1.4112. Logs are local/sow33800/late-reader-*.log. Verified upgrade, Results and rollback in a temporary fixture. Cort owns game testing and the current runtime has not been changed. The exact game shader causing the reflection remains open, with the full-size #da6 draw nominated by the saved order and live proof logging included.

### 2026-11-19 — Shadow of War reflection comparison continuity

Use Documents/NeuralAlias-ShadowOfWar-Reflection-18dea9e/02-INSTALL-ShadowOfWar.cmd. The 07a2004 capture proved the late placement suppressed the reflection flicker until the comparison timed out. PR 175 repairs the consecutive-screen sampling across alternating readers. The source regression fails before and passes after on the RTX. The 18dea9e package is in Documents/NeuralAlias-ShadowOfWar-Reflection-18dea9e. All 11 payload hashes, fixture upgrade from 07a2004, Results and exact rollback verified. Five focused assertions suites passed with the new runtime. Two additional tests using real NVIDIA DLAA passed with the normal fixture DLL. The route was kept after baseline recheck, delivered on 28 of 28 frames and matched 69,276 distinguishing pixels. The unrelated flash still causes fallback while DLAA continues. With the packaged NVIDIA DLL, those two automated renderers stalled during the first DLAA evaluation, before the Referee ran. Thread samples in the stalled fixture were inside nvngx_dlss.dll. The package retains the same NVIDIA DLL hash as Cort’s successful game capture. Test receipts are local/sow33800/rotating-pair-*.log. Cort retains gameplay validation.

### 2026-11-19 — Shared Referee recovery for DX11 and DX12

PR 176 at 270d7b6b uses one decision policy and adds DX12 rotating-lighting placement with independent screen proofs for both inputs. The focused Release set passed 7/7, including actual NVIDIA DX11 tests and the RTX DX12 screen fixture. The preceding Shadow of War package remains the game test artifact. See COMMS and the branch WORKLOG for exact evidence and scope.

### 2026-11-19 Pacific — Shadow of War overlay preferences

PR 177 (323a805e) on top of the shared Referee branch removes the Steady Distant Lighting switch and enables qualified correction automatically. The Replay checkbox is visible but disabled in native DX11 and remains off by default in DX12. Seven focused Release checks passed. The reported distant vibration remains open. See COMMS for the current evidence. Existing live package is 18dea9e.

### 2026-11-20 Pacific — Native DX11 jitter history and Shadow of War pixel audit

PR 178 (01f707b4) resets DLAA once on a confirmed change of sampling mode. Three focused RTX tests passed, preserving the late-reader regression. Saved menu evidence shows DLAA output on the orc's skin, armor, cloth and fur plus a sign reversal in castle brightness after DLAA. COMMS lists the affected captures and distinguishes this from live verification. No updated CMD.

### 2026-11-20 Pacific — Lighting evidence and jitter switch parity

PR 179 (0d8e38fe) takes native DX11 lighting proof before Home, as DX12 already does, and leaves Home in the separate full-screen capture. Two real-NGX RTX tests passed. PR 180 (a72c26d0) implements one jitter-switch history rule across DX11 and DX12, with a Release build and seven further regression passes. Live verification of the castle and a combined package remain open. Technical evidence is in COMMS and the branch WORKLOG.

### 2026-11-20 Pacific — Unified release line

Draft PR 181 into main is backed up on GitHub at astra/unified-main a30f16fe. It preserves the tested production sources through a72c26d0 and the main and integration journals. It is the convergence point for both routes, both graphics APIs and the shared Referee. Fable's final handoff and the combined automated verification precede the final merge. See COMMS for the overlap already identified.

### 2026-11-20 Pacific — Opus integration verified

Unified draft PR 181 includes Opus's Home texture fix and native DX11 Draw Replay with the recorded ownership guards. Runtime e4d0ab7d built in Release and passed all eight focused tests. Test details and remaining live verification are in COMMS and the integration WORKLOG at ed5c1115. The final runtime merge is held for Fable's handoff.

### 2026-11-20 Pacific — Referee jitter and lighting verification

Draft PR 183 adds brightness-invariant camera-free motion fitting and same-screen geometric jitter evidence. Both APIs retain a screen-proved lighting placement while testing a qualified follow-up correction in the same region. Runtime df467561 passed 11 focused tests, including 90.1 and 90.5 percent less displayed variation in the controlled flash footprint. The broader patch and the saved game intervals have separate results in COMMS. Matched live proof of a 90 percent reduction for both user-reported symptoms remains open. This draft is separate from the held unified main merge.

### 2026-11-20 Pacific — Shadow of War lighting test candidate

Draft PR 183 runtime db3f2788 follows residual reflections through verified screen regions and rejects a correction that creates an off-target flash. Five focused final CPU and RTX tests passed, and a separate broad 18-case run passed for the preceding runtime. Documents/NeuralAlias-ShadowOfWar-LightFix-db3f2788 is staged with an 11-hash manifest, verified update, Results and rollback. Game and installed package are still under Cort's control. Matched live verification remains open. See the new COMMS entry and branch WORKLOG.

### 2026-11-20 Pacific — Cort placed the lighting and jitter package on hold

Cort requires at least 90 percent verified reduction for the reported lighting and vibration before any further build or installer. The earlier db3f2788 candidate is withheld from use. There are no game or installer changes in this handoff. Astra is prioritizing the jitter signal on existing captures while Opus and Fable have the lighting evidence and remaining gap in COMMS.

### 2026-11-20 Pacific — Opus rotating-light fix integrated
PR 183 includes Opus's PR 185, scoped parity admission and continuity across an unchanged timed comparison, the jitter Reset, and the RCAS cap. Tested runtime is 7783d816. Real native DX11 DLAA stayed on the proved route past the old timeout and reduced visible flash variation 94.5 percent in the RTX fixture. Final focused CPU and DX12 checks also passed. See COMMS and the PR WORKLOG. Shadow of War live confirmation and a new installer remain pending.

### 2026-11-20 Pacific — Sniper Elite 4 corrected installer
Documents/NeuralAlias-SniperElite4-DX12-ddf55beb-SceneDiscovery supersedes the original ddf55beb package. Same runtime, installer and Results from 45eca490. The original Auto profile left DX12 scene discovery off. The corrected profile starts discovery with the shipped DLL and updates the old package cleanly. The live run had zero evaluations even though the Referee switch was enabled during the session. Corrected live gameplay is pending.

### 2026-11-20 Pacific — Sniper Elite 4 launch recovery
The ddf55beb-SceneDiscovery package crashed twice before core attachment. Use Documents/NeuralAlias-SniperElite4-DX12-31463de6-FactoryStart. The new profile waits for the game's first successful DXGI factory call to start the host, then enables scene discovery. The generated profile and shipped DLL pass the factory startup check, and upgrade and rollback were verified in a fixture. Game launch and delivery remain to be confirmed by Cort.

### 2026-11-20 Pacific — Sniper Elite 4 resolved-layer jitter
Cort confirmed that 31463de6 starts and delivers DLAA. The remaining menu windshield buzz returned between the steady reconstructed image and the final display. PR 183 b75bf44f corrects native DX12 subpixel jitter on qualifying depth-tested layers after reconstruction. Six focused regressions passed, including a stationary/moving RTX glint check. The reversible factory-start upgrade from 31463de6 passed with the shipped profile. Current test package is Documents/NeuralAlias-SniperElite4-DX12-b75bf44f-FactoryStart. The in-game windshield result awaits Cort. COMMS and the PR's WORKLOG record the evidence and limits.

### 2026-11-21 Pacific — Shared Referee recovery across returning scenes

PR 183 at 43cfc565. Both native APIs share the corrected lighting admission, damage comparison and motion-trial fallback. Eight focused tests passed, including real native DX11 DLAA, native DX12 rotating placement, adverse scenes, reused depth and a separate next-tool comparison. The older Shadow of War trace still qualifies with both phases present. See the dated WORKLOG and the November 21 decision table in docs/REFEREE-RECOVERY-AUDIT-20261119.md on PR 183. The new commit is on GitHub for Sol; the existing SE4 package is b75bf44f. Referee-driven jitter placement remains open.

### 2026-11-21 Pacific — Shutdown and Referee jitter on both native APIs

PR 183 at abb2d5f2. The equivalent original shutdown fixture fault was decoded to RetireSentinel during registry destruction, repaired in 0837359c and repeated successfully five times. The new shared Referee logic tries reduced viewport jitter against a proved normal-strength baseline and confirms the benefit on a normal-strength recheck. Both native adapters apply the scoped phase, and each preserves the original raster for qualified downstream composition. Eleven focused Release tests passed on this revision. Both proxies built. COMMS records the Fable handoff and worktree log. This revision is ready for source integration. The staged SE4 package remains b75bf44f.


### 2026-11-21 Pacific — Unified Referee merged; Pharaoh Dynasties staged

PR 181 merged to main at 393db03a with both Astra and Fable's fixes. Release 3e2eb03d is staged for Cort's first Pharaoh Dynasties run. The three CMDs are in Windows Documents. Installer and rollback passed in a temporary folder, with no game installation or launch. COMMS records the 12/13 RTX suite, Cort's acceptance of 89.8% in the companion lighting fixture, the unchanged 90% assertion, and the 32-bit transport evidence.

### 2026-11-21 Pacific — Three Kingdoms first campaign and automatic Referee

Three Kingdoms delivered DLAA in the captured campaign at both resolutions. The jitter-linked riverbank defect appears in reconstructed and displayed pixels. A hidden Referee was disabled and ran no checks. Original RCAS and automatic Route B Referee are now staged in both Total War installers at a072227a, PR 193. Seven automated Release checks and the Results tests passed. Upgrade and removal were verified in temporary game folders. Cort owns the live verification. See the November 21 COMMS entry and the PR WORKLOG.
