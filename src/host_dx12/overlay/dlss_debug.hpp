#pragma once
#include <cstddef>
#include <cstdint>

// NVIDIA's development DLSS DLL (DLSS SDK lib/Windows_x86_64/dev/nvngx_dlss.dll) draws its own
// debug views and reacts to its own hotkeys (DLSS Programming Guide 3.18 / 8.2: CTRL+ALT+F12
// cycles the views, CTRL+ALT+F9 cycles jitter configurations, CTRL+ALT+F6 accumulation,
// CTRL+ALT+F10 jitter negation/swap). This module gives the overlay a section for it:
//   * which nvngx_dlss.dll the game loaded (path, version, SHA-256 -> development / release),
//   * the on-screen indicator registry value, with an elevated on/off (UAC prompt),
//   * the hotkeys as buttons: the key combination is sent to the game with SendInput and held
//     for a few presents, and the overlay's input lock lets exactly those keys through,
//   * "Record": one folder with the next presented frames (raw + .bmp), DLSS's inputs/outputs,
//     a copy of the status log and a note with the current DLSS/HUD status lines.
// Nothing here changes rendering.
namespace na::dlss_debug {
struct Info {
    bool loaded = false;        // nvngx_dlss.dll is in the process
    wchar_t path[260] = {};
    uint64_t bytes = 0;
    char version[32] = {};      // file version "a.b.c.d" or ""
    int build = 0;              // 0 not hashed yet / unknown, 1 release, 2 development
    char sha256_16[17] = {};    // first 16 hex digits of the file's SHA-256 ("" until hashed)
    bool indicator_known = false;
    uint32_t indicator = 0;     // HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\ShowDlssIndicator
};
Info Query();                   // cached; refreshed every 2 s; the hash is computed once, off-thread
bool PressHotkey(int vk);       // CTRL+ALT+vk to the game (SendInput); released by Tick after ~4 presents
void Tick();                    // every present (menu open or closed): releases a held hotkey
bool SyntheticKeyAllowed(int vk); // input detours: let this key through the menu's input lock
bool SetIndicator(bool on);     // elevated reg.exe (UAC prompt); true = the prompt was shown
// One timestamped folder under LocalAppData\NeuralAlias\captures (…-dlssdebug-…): the next
// `frames` presented pictures (frameN-backbuffer.raw + .bmp + meta), DLSS's planes for the
// next `frames` evaluations, status-log.txt and debug-note.txt. Returns the folder.
bool Record(uint32_t frames, wchar_t* dir_out, size_t dir_chars);
void SetStatusLogPath(const wchar_t* path); // overlay: the per-process status log to copy
const wchar_t* LastRecordDir();             // "" until Record succeeded once
}
