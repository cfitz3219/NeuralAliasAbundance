# NeuralAliasAbundance action journal

## 1 - 2026-09-22 - scope approved and independent source created

User clarified DX12 broadly, two routes for games with/without existing DLSS, and Frame Generation placeholder. User then said Proceed. Announced4-8hour first-stage estimate and no attendance needed. Read original AGENTS/HANDOFF/WORKLOG,CMake,core/testinventory,third-partyREADME and existingbuildcache. No subagents. Created separate outputs/NeuralAliasAbundance only after approval. Copied temporal_jitter/frame_history/upload_cache headers and their tests unchanged, plus prior complete sanitized source-review archive asreference. RecordedSHA256provenance. No deployed game/mod files changed. Read MicrosoftprimaryDX12lifecycle/factorydocumentation; no sampleimplementation copied.

## 2 - independent build, contracts and GPU fixture

Implemented x64CMakeproject,/W4/WX, two-provider selection from evidence rather thanDLLpresence, resource frame/generation checks, reconstruction metadata validation, separatelyqualifiedNRgate and unavailableFG. Added deterministicDX12compute fixture with clean scene/depth/motion/compositedHUD outputs, knownmotion andjitter, explicitresourcebarriers and boundedofflineGPUreadback. Added tests for missing/staledata, providerpriority, backendunavailability, cuts andhistoryrecovery. AddedCLIhardwarestatus.

Configured with installedVS18/MSVC19.51.36257.0 andWindowsSDK10.0.26100.0. FirstReleasebuildpassed; first6CTestspassed. GPUtestchecked379460pixels in8frames over320x180and257x145;debuglayerenabled/noerrors. No performance claims or neural inference. Readback waits deliberately confined to offlinefixture.

## 3 - management window and system forwarding

AddednativeWindowssettingspreview withthree routepreferences (automatic/existingupscaler/adapter), explanatorytext anddisabledDLAA/NR/FGcontrols. Previewdoesnotpersistorinstall. AddedlazyDXGIsystemforwarderexports, noDllMaininitialization, ABIstatusquery. NamedNeuralAliasDXGI.dll toavoidaccidentalautoload. Explicitloadingtestvalidatesfactory/1/2, DX12device/queue/swapchaincreation, presentationtest,resize, objectretirement andDLLunload. Notfullsystemexportcoverageorhooking. Releasebuildpassed;all8CTestspassed. Settingssmokeishiddenprogrammaticvalidation,notvisualQA. NoMinHook/ImGui/NVIDIAdependencydownloadrequiredyet.

## 4 - user selects Anno1800 and read-only audit

Asked asynchronously forfirstinstalledDX12gamewhilecontinuingindependentwork. UserselectedAnno1800, reportsbothAPIsandnoDLSS. Acknowledgedadapterroutepriority. ReadSteamlibrary/targetmanifest, targetexecutablefolder andknownDocumentsconfigfolder. InitialnonredirectedDocuments/Ubisoftpathchecksreturnednoentries; resolvedactualWindowsMyDocumentsviaEnvironmentAPI. Onebatchedcommandexited1solelybecausefinalGet-ProcessfoundnoAnno;follow-upexplicitconditionalconfirmedclosed. Didnotinterpretthatastool/buildfailure.

MSVCdumpbinverifiedx64andDXGIfactory/D3D11/delayedD3D12imports;hashedexecutable. Steamapp916440build16367353;configDeviceTypeDX12,FSRUpscaling0. Noexistingproxyfileinlistedfolder; nostaticNGXmatch,notproofofabsenceofdynamiccalls. Noaccount/savefilesread. SearchedprimaryreferencesforAnno;Intel/Ubisoftarticleprovidesdepthprepasscontextonly. NoresourcesidentifiedorlivegameAPIverified. DetailsinANNO-1800.md. ManagerstatusrunconfirmedRTX5070TiandIntelUHD770available.

## 5 - milestone documentation and reproducible build entry point

AddedREADME,AGENTS,HANDOFF,provenance/credits,Annoreport,gitignoreandbuild.ps1thatdiscoversinstalledVSthroughvswhere. Reportsseparateactualtestsfromfutureenhancementwork. Noeffectsconnectedtostandalonehostyet. OriginalAlieninstallationandconfigurationunchanged. Finalscript/evidence/provenanceverificationfollowsbelow.

## 6 - collaboration pause and Route B acknowledgment

Before interruption, build.ps1 completed successfully and all eight CTests passed. Further provenance/repository verification was not completed. User paused work and explicitly adopted Claude's division of labor, requesting agreement acknowledgment, target selection, status update and a copy/paste response only. Read outputs/COLLABORATION.md, HANDOFF and WORKLOG. Git status at repository root reports no Git repository; no initialization or branch switch performed because Claude owns baseline creation.

Retained user-selected Anno 1800/DX12 as Route B target. Updated status board and corrected stale core-only inventory with existing diagnostics/settings/limited-forwarder facts and limitations. Proposed separate worktrees because parallel branch checkouts cannot safely share one working directory. No shared-interface changes proposed or performed; no Route A code, game files or deployment records touched. Updated newest-first HANDOFF. First multi-file documentation patch failed context verification against WORKLOG; re-read all affected files and confirmed no changes, then applied corrected patch. No GPU tests/game sessions run during pause acknowledgment. Next requires user go-ahead plus checkout coordination; CPU discovery design precedes any scheduled GPU inspection.

### 2026-10-24 — Astra — Gate 2 Windows RTX validation

User authorized local Windows build/hardware tests, deferred Argon, and explicitly prohibited game deployment. Read MASTER-PLAN, COMMS, AGENTS, CLAUDE, COLLABORATION, HANDOFF, WORKLOG and recovery notes. Clean main at 4839fc3 (runtime tree from merged 89b93b6); pull --rebase reported up to date. No subagents or production changes.

Fresh isolated build: build/gate2-rtx-20261024. VS Community 2026 18.11.1; MSVC 19.51.36260.0; CMake 4.3.1-msvc1; Windows SDK 10.0.26100.0. NVIDIA RTX 5070 Ti, driver 621.01. Default local SDK root found NGX headers/static library/runtime, Optical Flow and PCL; vendored NVAPI enabled. SDK presence/build success does not qualify those optional features operationally. DLSS DLL version 310.9.16.0; source and test copy SHA256 D00B5B0AB29C3CA101B70C3A5ECB3A01B7EF33766A3383A28A18623083E6123E.

Initial configure could not discover Python and omitted A3; corrected cache using the existing bundled Python at C:/Users/Cort/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe. No installation or source edit needed. Release build succeeded. Explicit CTest inventory confirmed host_dlss_real_test and na.fixture.reference registered. Full verbose serial CTest: 30/30 passed, zero skips, 13.98 seconds. A3 ran all 10 cases. Hardware-bearing tests reported RTX 5070 Ti, not WARP. DX12 fixture checked 758920 pixels. Queue timing regressions passed. FFX exposure debug assertions enabled and passed on success and failed-evaluate paths.

Real NGX initialized successfully (DLSS available, no driver update needed). Synthetic zone plate 1280x720 -> 1920x1080, 128 evaluations, one feature created, zero pending/leaked/untracked at pre-shutdown stats. Bilinear PSNR 11.25 dB; jitter +1 DLSS 8.75 dB; expected jitter -1 DLSS 17.68 dB (+6.43 dB). Optimal settings queries succeeded, including dynamic ranges; querying a range is not evaluating a DRS ramp. Backend/interception regression tests use fake NGX/stand-in FFX/XeSS with real D3D12 resources, not actual game runtimes.

Raw logs remain ignored/local: local/gate2-rtx-20261024/{hardware.txt,configure.log,configure-python.log,build.log,test-inventory.log,ctest-verbose.log}; build/Testing data resides under the isolated build directory. Test runtime DLL copy is confined to that build. No game files read or changed, no deployment, no intentional physical device loss. Argon/delta audit, real-runtime DRS evaluation, record-ahead visual/performance behavior, FSR/XeSS game conventions and physical device-loss recovery remain unqualified. Gate 2 remains open. Initial estimate 20–45 minutes; tool build/test execution finished in minutes with no user attendance required.
