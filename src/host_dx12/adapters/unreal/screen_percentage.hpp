#pragma once
// Unreal adapter phase 2: change the render resolution LIVE through the
// engine's own r.ScreenPercentage, the setting the game's DLSS menu changes.
//
// Evidence (Cort's PBZ logs, 2026-10-29): the engine asks DLSS for its sizes
// once at startup (six 1000x1000 queries), then changed its render size live
// from its own menu (1146x480 -> 2295x961 -> 1996x836 -> 1720x720) with no new
// queries. So the size comes from the engine's screen-percentage cvar each
// frame; NGX answers cannot move it after startup.
//
// How: the static TAutoConsoleVariable<float> for r.ScreenPercentage is found
// in the game executable (its UTF-16 name, the static initializer's
// RIP-relative LEAs; layout [0] vtable, [8] IConsoleVariable*, [16]
// TConsoleVariableData<float>* -> ShadowedValue[0] game thread, [1] render
// thread). A candidate is accepted only if exactly one validates AND its
// current value matches the render size the game is actually feeding DLSS.
// A request writes both shadowed values (aligned 32-bit stores, as the cvar
// system's own set does) and is reported Confirmed only when the game's next
// DLSS creation uses the expected render size. A successful write is not the
// success witness. If the game itself changes the value afterwards (its menu),
// the adapter reports that and does not fight it.
#include <cstdint>

namespace na::unreal {

enum class SpState : uint32_t {
    Unknown = 0,    // not searched yet
    NotFound = 1,   // not an Unreal executable, or the cvar could not be located unambiguously
    Ready = 2,      // located; no request yet
    Applying = 3,   // written; waiting for the game's DLSS to be re-created at the new size
    Confirmed = 4,  // the game's DLSS input now has the expected size
    NotApplied = 5, // written, but the expected size was not observed in time
    GameChanged = 6,// the game changed the value itself after our request
    Refused = 7,    // request rejected (located value does not match the render size, or busy)
};

struct SpStatus {
    SpState state = SpState::Unknown;
    bool available = false;      // located and validated: requests are possible
    float current = 0.f;         // ShadowedValue[0] now
    float requested = 0.f;
    uint32_t expect_w = 0, expect_h = 0; // render size the request should produce
    uint32_t seen_w = 0, seen_h = 0;     // game's latest DLSS creation size
    uint64_t request_id = 0;
    uint32_t candidates = 0;     // validated candidate objects (1 = unambiguous)
    char reason[200] = "";
};

// Starts the locator thread (no writes). Safe to call more than once.
void StartScreenPercentage();
// Asynchronous: writes `percent` and watches the game's next DLSS creation for
// ceil(out * percent / 100). Returns false (state Refused) when unavailable or busy.
bool RequestScreenPercentage(float percent, uint32_t out_w, uint32_t out_h);
SpStatus GetScreenPercentageStatus();

// Expected Unreal render extent for an output extent and percentage (the
// engine rounds up: 3440 * 58% = 1995.2 -> 1996, matching Cort's log).
uint32_t ExpectedExtent(uint32_t out, float percent);

} // namespace na::unreal
