# Ghost of Tsushima Gate 2 validation — 2026-10-24

User authorized reversible deployment and separate FSR/XeSS sessions, with native DLSS unselected. Argon remains deferred. Initial estimate: 45–90 minutes, user attendance potentially needed for game controls.

## Installed build and interface inspection

Steam app 2215430, build 23879181; executable 1053.9.0623.1807. Local install: D:/SteamLibrary/steamapps/common/Ghost of Tsushima DIRECTOR'S CUT. Executable SHA256 ED0931B6F6D3CA3874BAF39EBC16D95FF0E93296891182F96C3A790DA7109F63.

- amd_fidelityfx_dx12.dll file version 1.0.1.41314. Read-only ffxQuery GET_VERSIONS returns providers 3.1.4 (0xf5a5ca1e00c01004) and 2.3.3 (0xf5a5ca1e00803003). Actual selected provider remains to be observed.
- libxess.dll file version 1.3.0.28; xessGetVersion returns 1.3.0.
- Game imports all five ffx API entry points and XeSS create/init/execute/destroy/velocity-scale functions. Both DLL names match the module watchers. These are API-based paths, not statically linked FSR.
- Compared upscale create/dispatch field ordering against AMD SDK v1.1.1 and XeSS init/execute layout against Intel v1.3.0. Existing declarations match these inspected structures; exports and source layouts alone do not qualify runtime inputs.
- Game Streamline is 2.4.0; DLSS SR runtime is 3.7.0.0. The SDK 310.9.16 runtime has NOT replaced the game's DLL. Earlier standalone hardware results apply to 310.9.16, not automatically to this runtime.

References: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.1/ffx-api/include/ffx_api/ffx_upscale.h and https://github.com/intel/xess/blob/v1.3.0/inc/xess/xess_d3d12.h . Full hashes/imports/exports and runtime query output are local/gate2-ghost/.

## Preparation and discovered startup gap

The Streamline proxy lacked the FSR/XeSS watcher startup implemented in the generic host. Added startup with observe defaults and explicit replacement opt-ins, plus five-second worker status counters for hooks, contexts, calls, replacements, fallbacks, backend success and errors. No GPU waits/readbacks added. Candidate 1d82b1b on astra/g2-ghost-validation; draft PR #19. Fresh Release build, all 30 CTests (including real NGX), and deployment/rollback script tests pass. These do not yet test successful game startup.

Deployment uses scripts/deploy-sl-proxy.ps1 and its hash-checked manifest in local/gate2-ghost. Original Streamline is renamed sl_interposer_real.dll and separately copied to backups/sl.interposer.dll. Original SHA256 AA20B751432B1BA19A06379117A4ADA8A8BA37C882E06A6203DD52FC8246BB1F. User NeuralAlias settings and discovered game registry settings exported to backups; no saves modified. Local rollback.ps1 invokes the hash-checking rollback. Native NVIDIA/AMD/Intel runtime DLLs are unchanged. Manifest verified deployed proxy, original and INI hashes.

First launch triggered Steam's PlayStation PC SDK prerequisite installer; Windows Installer reports successful 2.50.00.10 installation. Game then reached PreInit but had no visible window or injector log. Stopped this launch and its game-path crash helpers, preserving startup-active-game.log. Deployment update initially refused because the crash helper remained running; stopped only the matching game-path helper, then switched to forwarding-only mode for startup isolation. Game sessions and visual/performance findings are pending; no replacement success claimed.

Correction: the later fully enabled launch reached the game. The earlier launch was stopped too early to establish a hang. On resume, native DLSS evaluations were active in the main menu despite the prior launcher selection; FSR/XeSS request and injector counters were zero. This menu interval is excluded from FSR replacement qualification.


## Live session results (2026-10-24, local time)

FSR: in-game Display menu visually confirmed FSR 3.1.4, 3440x1440 HDR, VSync on and dynamic-resolution target 60 (the prior launcher Off setting did not govern this session). PID 23424 reached 22,810 FSR dispatches, all replaced, zero fallbacks; matching injector evaluations, one feature, zero rejections, NGX success 0x1. Cort reported no obvious visual issues. Sampled cinematic/gameplay images were coherent; no controlled temporal-quality or native-performance comparison was performed. Overlay recognized FSR GAME CREATED but retained SOURCE UNKNOWN for DLSS because initial native-menu creations remain in historical evidence; this is not a replacement failure.

XeSS: selected XeSS in-game, applied and confirmed it; native DLSS was not selected. The first process observed XeSS without replacing it because its startup configuration had xess.replace=0. Exited normally, updated the hash-checked deployment to xess.replace=1/fsr.replace=0, verified manifest hashes, and relaunched. This restart was needed to load the replacement configuration; merely changing the game method does not enable it. Cort questioned the restart; explained and received explicit direction to continue.

Fresh PID 45240: at 03:39:00, 734/734 XeSS calls replaced; at 03:40:05, 7,936/7,936 replaced with zero fallbacks, zero rejected submissions, one injector feature and NGX result 0x1. FSR contexts/dispatches remained zero; total NGX evaluations matched injector evaluations. Continued the saved game into the beach sequence. Overlay Details visibly showed XeSS game integration observed, XeSS GAME CREATED, DLSS INJECTOR (only injector-origin SR calls observed), and FSR LIBRARY ONLY. This corroborates the logs and distinguishes replacement from native DLSS. Registry UpscaleMethod=3 agrees with the prior visually confirmed XeSS selection; FrameGen/DLSSG both 0.

Sampled XeSS gameplay rendered characters, terrain, reflections, particles and smoke without obvious full-frame corruption. Stationary gameplay samples were approximately 81–84 FPS; overlay 83 FPS/12.1 ms, status 83.8 FPS/11.93 ms. Cinematic samples 81–95 FPS. These differing scenes/settings are not an FSR-vs-XeSS benchmark. DLSS GPU timing was explicitly unavailable in the overlay; logged 0.00 is not zero cost. NVIDIA diagnostic text identified DLSS 3.7.0, generic app, preset F, 3440x1440 -> 3440x1440 in sampled views; this does not establish all DRS extents or quality modes. No physical device-loss, extended stability, fast camera-motion/HUD or quantitative image-quality qualification claimed.

Evidence retained locally in fsr-session/status-{initial,overlay,gameplay,final}.log and xess-session/status-gameplay.log under local/gate2-ghost. Game remains running with XeSS selected and replacement enabled, original DLL backup and rollback script intact. Gate 2 remains open for review, deferred Argon audit and remaining quality/lifecycle coverage. Both installed-build interceptor routes have now demonstrated successful real DLSS replacement; FSR's mixed-history overlay limitation is retained explicitly.
