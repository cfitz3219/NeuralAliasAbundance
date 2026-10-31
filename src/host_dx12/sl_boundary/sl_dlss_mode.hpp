#pragma once
// Streamline DLSS mode layer (any engine that sizes its render through Streamline).
//
// Streamline games choose their render size by asking
//   slDLSSGetOptimalSettings(DLSSOptions{mode, outputWidth, outputHeight, ...}, DLSSOptimalSettings&)
// and then hand the mode to slDLSSSetOptions(viewport, DLSSOptions). Both are
// plugin functions resolved through slGetFeatureFunction(kFeatureDLSS, ...),
// which the SL boundary already hooks. This layer wraps them:
//   - GetOptimalSettings: with a request active, the GAME's requested mode is
//     replaced by ours in a copy of its options (its own struct is untouched),
//     so whatever the game does with the answer, it gets our mode's size.
//     When pin_bounds is set, the returned dynamic-resolution bounds are
//     pinned to that size (min = max = optimal): a game whose dynamic
//     resolution re-reads the bounds every step converges on our size.
//   - SetOptions: per-mode preset fields get our preset (any time a preset is
//     requested). The MODE is rewritten only after the game has taken a size
//     from a rewritten GetOptimalSettings call since the request: a DLSS
//     feature labelled for a smaller mode than the game actually renders
//     would be wrong, so the label follows the size, never leads it.
// Whether this changes the picture live depends on WHEN the game asks: the
// cadence (calls per second, calls since the request) is reported so the
// Apply selector can decide, and success is only ever the confirmed render size.
#include <cstdint>

namespace na::slb::dlss {

// sl::DLSSMode values.
constexpr uint32_t kSlOff = 0, kSlMaxPerformance = 1, kSlBalanced = 2, kSlMaxQuality = 3, kSlUltraPerformance = 4,
                   kSlUltraQuality = 5, kSlDlaa = 6;

struct ModeRequest {
    bool own_mode = false;   // replace the game's requested mode
    uint32_t sl_mode = 0;    // sl::DLSSMode to request
    bool pin_bounds = true;  // min = max = optimal in the returned settings
    bool own_preset = false; // write `preset` into every per-mode preset field
    uint32_t preset = 0;     // sl::DLSSPreset (10 = J ... 13 = M)
};
// Takes effect on the game's next call. Each call starts a new generation.
void SetModeRequest(const ModeRequest& request);

struct ModeState {
    uint32_t optimal_hooked = 0, set_hooked = 0;
    uint64_t optimal_calls = 0, set_calls = 0;
    uint64_t generation = 0;                 // request generation
    uint64_t optimal_calls_this_generation = 0, rewritten_this_generation = 0;
    uint64_t last_optimal_tick = 0, first_optimal_tick = 0; // GetTickCount64
    uint32_t game_mode = 0xFFFFFFFFu, sent_mode = 0xFFFFFFFFu;  // last GetOptimalSettings request
    uint32_t out_w = 0, out_h = 0;                               // output size the game asked about
    uint32_t optimal_w = 0, optimal_h = 0;                       // answer delivered to the game
    uint32_t min_w = 0, min_h = 0, max_w = 0, max_h = 0;         // bounds delivered to the game
    uint32_t set_game_mode = 0xFFFFFFFFu, set_sent_mode = 0xFFFFFFFFu; // last SetOptions
    bool size_followed = false;              // the game took a rewritten size this generation
    int32_t last_result = 0;
    char reason[160] = "";
};
ModeState GetModeState();

// Boundary plumbing (called by sl_boundary.cpp): a kFeatureDLSS function the
// game resolved, and a one-time lookup through the hooked slGetFeatureFunction.
void OnFeatureFunction(const char* name, void* function);
using LookupFn = int32_t(__cdecl*)(uint32_t feature, const char* name, void** function);
void LookupOnce(LookupFn lookup);

// Pure helpers (unit-tested).
uint32_t SlModeForDlssMode(uint32_t dlss_mode); // settings::DlssMode -> sl::DLSSMode
bool WaitInstalledForTests(uint32_t timeout_ms);
void ResetForTests();

} // namespace na::slb::dlss
