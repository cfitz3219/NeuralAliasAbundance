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
