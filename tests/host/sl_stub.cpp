// Reconstructed 2026-10 (C3). Stand-in for the game's real Streamline
// interposer, built as sl_interposer_real.dll (the name deployment gives the
// renamed original). Exports a handful of sl* entry points with sentinel
// behaviour so sl_proxy_test can prove forwarded and wrapped calls arrive
// with their arguments intact. Deliberately does NOT export slSetTag, so the
// proxy's lazy forwarder for it must fail cleanly when resolved.
#include <windows.h>
#include <cstdint>

namespace {
struct State { uint32_t inits, set_consts, get_settings, set_constants, evaluates; uint32_t last_feature, last_frame, last_id; const void* last_ptr; };
State g{};
} // namespace

#define SL_EXPORT extern "C" __declspec(dllexport)

SL_EXPORT bool __cdecl slInit(const void* prefs, int app_id) { ++g.inits; g.last_ptr = prefs; g.last_id = static_cast<uint32_t>(app_id); return true; }
SL_EXPORT int __cdecl slShutdown() { return 0x51; }
SL_EXPORT int __cdecl slIsFeatureSupported(uint32_t feature, const void*) { g.last_feature = feature; return static_cast<int>(0x5100 + feature); }
SL_EXPORT int __cdecl slGetNewFrameToken(void** token, const uint32_t* frame_index)
{ static uint32_t tok = 0xF7A3E; if (token) *token = &tok; g.last_frame = frame_index ? *frame_index : 0xFFFFFFFFu; return 0; }
SL_EXPORT bool __cdecl slSetFeatureConstants(uint32_t feature, const void* consts, uint32_t frame_index, uint32_t id)
{ ++g.set_consts; g.last_feature = feature; g.last_ptr = consts; g.last_frame = frame_index; g.last_id = id; return true; }
SL_EXPORT bool __cdecl slGetFeatureSettings(uint32_t feature, const void* consts, void* settings)
{ ++g.get_settings; g.last_feature = feature; g.last_ptr = consts; if (settings) *static_cast<uint32_t*>(settings) = 0x5E77; return true; }
SL_EXPORT bool __cdecl slSetConstants(const void* constants, uint32_t frame, uint32_t view)
{ ++g.set_constants; g.last_ptr = constants; g.last_frame = frame; g.last_id = view; return true; }
SL_EXPORT bool __cdecl slEvaluateFeature(void*, uint32_t feature, uint32_t frame, uint32_t view)
{ ++g.evaluates; g.last_feature = feature; g.last_frame = frame; g.last_id = view; return true; }

SL_EXPORT const State* __cdecl SlStubState() { return &g; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h); return TRUE; }
