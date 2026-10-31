// A54: shared host bootstrap for proxy DLLs that carry a game adapter
// (the DXGI proxy for non-Streamline titles such as Anno 1800; the
// sl.interposer proxy keeps its own Route A-specific init).
//
// Activation is ini-gated exactly like the SL proxy: `<ini_name>` next to
// the DLL with `[host] enabled=1`. Without it the proxy stays a pure
// forwarder with zero hooks. Everything runs on an init thread spawned from
// DllMain (no work under the loader lock): pin the module, hooking core,
// settings, overlay scale/fonts, adapter prepare, overlay Start (which owns
// the swap-chain/queue hooks and the discovery thread), adapter start.
#pragma once

#include <windows.h>

#include <cstdint>

namespace na::host {

// Spawns the init thread when the ini enables the host. Returns true when a
// thread was started (the ini exists and enables the host).
bool StartFromIni(HMODULE self, const wchar_t* ini_name, const char* default_tag);

struct Status {
    uint32_t host_enabled;    // ini said enabled=1
    uint32_t host_started;    // core attachment succeeded
    uint32_t early_gate_expired; // early queue capture not guaranteed; late attachment remains active
    uint32_t init_complete;   // worker completed menu/adapters/logger startup
    uint32_t adapter_prepared;
    uint32_t adapter_started;
    // A61: Route A services in a NON-Streamline DLSS game ([route_a] enabled=1):
    // NGX core hooked before the game reaches it, DLSS/NR/FG runtimes wired,
    // Route A menu pages, presenter (Frame Generation) enabled.
    uint32_t route_a_enabled;
    uint32_t route_a_started; // NGX core hooked and module watch running
    char adapter_name[32];
    char tag[32];
    char error[96];
};
Status GetStatus();

// G3-03: called by the proxy's DXGI factory exports before forwarding. With
// [observe] producers=1 OR Route A overlay enabled (early_install defaults to 1),
// a caller other than the
// init thread waits up to 3 s until producer installation was attempted, so
// early swap-chain queue bindings are captured as well as producer objects.
void WaitForObserveInstall();

} // namespace na::host
