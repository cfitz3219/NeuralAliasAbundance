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

## 2026-10-24 — Ghost Gate 2 preparation and resumed live session

Cort authorized Ghost of Tsushima deployment and separate FSR/XeSS sessions, native DLSS unselected and Argon deferred. Runtime query, import/export inventory and backup/manifest details are in docs/GATE2-GHOST-VALIDATION.md; private evidence is local/gate2-ghost in the primary checkout. Fixed the missing Streamline-path FSR/XeSS watcher startup and added worker-thread replacement/fallback/backend counters in 1d82b1b, draft PR #19. Fresh Release and 30/30 CTests passed; deployment script checks passed after retrying with its required Proxy/Stub arguments. Initial PowerShell brace expansion failed parsing and was corrected; an attempted AMD raw-header URL returned 404, corrected through the official repository tree. No third-party SDK/runtime files committed.

Correction to initial startup diagnosis: forwarding-only launch reached the launcher after about 30 seconds, and the later fully enabled launch also reached the game. The first launch was stopped too early to establish a hang. Steam's PlayStation SDK installer completed successfully. The game-path crash helpers briefly prevented deployment updates; the script correctly refused until they exited or the specifically matched helpers were stopped.

Launcher showed FSR 3.1.4, HDR at 3440x1440, Frame Generation off; changed dynamic resolution from target 60 to Off, leaving Quality selected. Saved launcher settings, exited and redeployed FSR replacement INI. User stopped Computer Use with physical Escape before gameplay verification; stopped input and reported partial state. Cort then explicitly resumed and offered to load gameplay with FSR. On resume, actual main-menu evidence contradicted the launcher selection: native NGX evaluates increased while FSR and XeSS requests and injector evaluations were zero. Communicated this immediately; it is not evidence of replacement. Hooks are present (FSR 5, XeSS 7). Awaiting user's gameplay/FSR selection before taking controls. No visual-quality or performance benefit is claimed from menu screenshots or counters.
