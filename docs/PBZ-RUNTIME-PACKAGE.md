# Phantom Blade Zero runtime package — 2026-10-29

User owns game testing. This task prepares a local installer; it does not launch or modify the actual game.

## Implemented

- FG preset C (NVAPI value 3) in settings, overlay and driver-profile writer. Existing TLOU support remains; other games explicitly opt in with deployment `[fg] profile_override=1`. DXGI Route A now initializes this service too.
- Ultra Performance is exposed in the Picture menu and accepted by the TLOU Apply adapter. Backend support already existed, but the UI previously clamped mode 4 to Performance and omitted its card. The earlier claim that support was complete was too broad.
- Optional `[settings] scope=PBZ` stores preferences separately under `%LOCALAPPDATA%\NeuralAlias\PBZ`, preserving other games' global preferences.
- `NAProfile` installer helper uses the same per-application NVAPI transaction as the runtime. It supports read-only status, apply and restore. Only FG override enable and FG preset are changed. Prior user values are preserved; inherited/predefined values are restored by removing our override. No global driver profile is changed.
- `scripts/deploy-pbz.ps1` verifies package hashes and NVIDIA signatures, backs up all replaced files, records a manifest before mutations, stages game writes, and rolls back a failed deployment. Rollback preserves game-update conflicts for inspection rather than destroying them. Changed menu preferences are archived before restoring their original state.

## Actual installed game inspection

Root: `C:\Program Files (x86)\Steam\steamapps\common\PBZ`.
The actual `platform\steam\PBZ.exe` (511,072,344 bytes) imports `dxgi.dll`. `steam\PBZ.exe` is a small launcher and does not; it is not modified.

Original plugin DLLs: SR/RR/FG 310.9.1.0, Streamline 2.14.1.0. Read-only NVAPI query found PBZ.exe's FG preset = 2 (B). No profile settings were changed during preparation.

Package payload: NVIDIA-signed SR/RR/FG/NR 310.9.16.0. SR comes from the installed NVIDIA SDK; RR/FG come from this PC's NVIDIA NGX downloaded runtime cache. NR matches the current NVIDIA RTX Remix `rtx-remix-ngx_sdk_dlnr@2` package byte-for-byte (SHA256 `2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F`). Local payload and backups are never committed to Git.

Official references:
- https://github.com/NVIDIA/DLSS/releases
- https://github.com/NVIDIA/nvapi/blob/main/NvApiDriverSettings.h
- https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/packman-external.xml
- https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/scripts-common/packman/external_config.packman.xml

## Deployment behavior

Install DXGI proxy, host ini and NR beside the actual executable. Replace the existing SR/RR/FG plugin files with their signed current versions, and add NR to the DLSS plugin search directory. Keep the game's Streamline files untouched.

Request FG preset C before launch, with a targeted driver backup. The game continues to own native FG, DLSS resolution and its current preset. NeuralAlias's SR override starts off, with Ultra Performance/M retained as its selectable defaults. NR starts off; Home > Neural Rendering turns it on for the user's comparison. J2 discovery, jitter injection, alternative-upscaler replacement and our separate FG presenter remain off.

Menu changes to the FG preset require a restart. The saved driver request is not proof of the model selected by the runtime. Native-game controls remain the means of changing PBZ resolution; this task does not fabricate a TLOU-style PBZ engine Apply adapter.

Logs: `%LOCALAPPDATA%\NeuralAlias\PBZ\status.log`, `startup.log`, `observe.log`; installer receipts and original-file backups are inside the package's `state` folder. Keep that folder until rollback is no longer needed.

## Validation and limits

Release DLL/helper build; focused profile, settings persistence, NGX interception and NR depth tests. Profile regression uses an in-memory NVAPI substitute: no driver writes. Installer regression uses a temporary fake game and fake profile helper, covering successful install, exact rollback, mutable preferences, reinstall, changed-game refusal and partial-install failure. Real profile lookup is read-only.

No game launch, in-game image-quality claim, NR performance measurement or proof that FG C is selected in gameplay. Those are the user's live test. Initial new test build failed because test::finish required a label; fixed before the passing run.
