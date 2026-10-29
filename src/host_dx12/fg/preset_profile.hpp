#pragma once
#include <cstdint>
namespace na::fg::preset {
struct Status {
    bool available = false, pending = false, restart = false;
    uint32_t requested = 1, stored = 0;
    int32_t result = -1;
};
// Published NVIDIA DRS per-application setting. This is a driver request,
// not evidence of which model the NGX runtime selected. Never edits global DRS.
void Initialize(); // init thread, TLOU I or explicit per-game profile_override; read/persist before our NGX preload
void Request(uint32_t preset); // menu thread: queue only, no DRS I/O
Status GetStatus();
}
