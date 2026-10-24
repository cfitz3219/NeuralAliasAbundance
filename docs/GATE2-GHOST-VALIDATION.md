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

