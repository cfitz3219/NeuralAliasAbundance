#pragma once
// Located compatible Unreal engine setters, queued at the normal game-thread console sink.
// No direct shadow stores. Success requires coherent SR/RR evaluations, with or
// without feature recreation. Admission uses supported ABI relationships, not a game hash.
#include <cstdint>

namespace na::unreal {

enum class SpState : uint32_t {
    Unknown = 0,    // not searched yet
    NotFound = 1,   // not an Unreal executable, or the cvar could not be located unambiguously
    Ready = 2,      // located; no request yet
    Applying = 3,   // queued/applied; waiting for actual DLSS evaluations at the new size
    Confirmed = 4,  // the game's DLSS input now has the expected size
    NotApplied = 5, // written, but the expected size was not observed in time
    GameChanged = 6,// the game changed the value itself after our request
    Refused = 7,    // request rejected (located value does not match the render size, or busy)
    Cancelled = 8,  // withdrawn (selection changed, NeuralAlias or our resolution control switched off)
};

struct SpStatus {
    SpState state = SpState::Unknown;
    bool available = false;      // located and validated: requests are possible
    float current = 0.f;         // ShadowedValue[0] now
    float requested = 0.f;
    uint32_t expect_w = 0, expect_h = 0; // render size the request should produce
    uint32_t seen_w = 0, seen_h = 0;     // game's latest observed DLSS evaluation size
    uint64_t request_id = 0;
    uint32_t candidates = 0;     // validated candidate objects (1 = unambiguous)
    char reason[200] = "";
};

// Starts the locator thread (no writes). Safe to call more than once.
void StartScreenPercentage();
// Asynchronous: queues engine Set(percent), disables DRS and checks frames for
// ceil(out * percent / 100). Returns false (state Refused) when unavailable or busy.
bool RequestScreenPercentage(float percent, uint32_t out_w, uint32_t out_h);
SpStatus GetScreenPercentageStatus();
// Withdraws a running request (its confirmation too). The value stays as it is.
void CancelScreenPercentage();
// Queues game-thread restoration of the setting it had before our
// first write, but only if our value is still in place (a change the game made
// itself is kept). Returns true if restoration was queued.
bool HandBackScreenPercentage();

// Expected Unreal render extent for an output extent and percentage (the
// engine rounds up: 3440 * 58% = 1995.2 -> 1996, matching Cort's log).
uint32_t ExpectedExtent(uint32_t out, float percent);

} // namespace na::unreal
