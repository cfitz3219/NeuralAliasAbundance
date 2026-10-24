// C7b (MASTER-PLAN Phase 2): XeSS call interception (libxess.dll, D3D12).
//
// Hooks the game's libxess.dll exports in the game's process (the DLL file is
// never touched): context creation, Init (output size, quality, flags), the
// jitter/velocity/exposure scale setters and Execute. Each Execute is recorded
// with everything DLSS needs, and either
//   * Observe (default): passed on to XeSS, or
//   * ReplaceWithDlss: fed to the C6 DLSS backend, writing XeSS's own output
//     texture, with XeSS skipped for that frame. Anything DLSS cannot do
//     faithfully (no depth, sub-rectangle inputs, unknown context, DLSS
//     unavailable) falls back to XeSS for that frame with a reason.
// Licence note (Cort's decision, 2026-10-24): full replacement is implemented
// knowingly despite the XeSS SDK licence's no-alteration clause.
// XeSS's documented resource states (inputs NON_PIXEL_SHADER_RESOURCE, output
// UNORDERED_ACCESS) are exactly DLSS's, so no barriers are needed.
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace na::dlss {
struct EvalDesc;
}

namespace na::xess {

enum class Mode : uint32_t { Observe = 0, ReplaceWithDlss = 1 };
void SetMode(Mode m);
Mode GetMode();

struct Observation {
    uint64_t serial = 0;
    void* context = nullptr;
    bool context_known = false; // xessD3D12Init for this context was observed
    ID3D12Device* device = nullptr;
    uint32_t init_flags = 0;    // xess_init_flags_t
    int32_t quality = 0;        // xess_quality_settings_t
    uint32_t out_w = 0, out_h = 0;
    float jitter_scale_x = 1, jitter_scale_y = 1;
    float velocity_scale_x = 1, velocity_scale_y = 1;
    float exposure_multiplier = 1;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* velocity = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* exposure = nullptr;
    ID3D12Resource* responsive = nullptr;
    ID3D12Resource* output = nullptr;
    float jitter_x = 0, jitter_y = 0, exposure_scale = 1;
    bool reset = false;
    uint32_t in_w = 0, in_h = 0;
    bool nonzero_bases = false; // any input/output base coordinate other than (0,0)
};

bool ToDlss(const Observation& o, na::dlss::EvalDesc* out, const char** reason);

uint32_t InstallForModule(HMODULE module);
bool IsXessModuleName(const wchar_t* name, size_t len);
bool StartModuleWatch();

bool LastObservation(Observation* out);

struct Stats {
    uint32_t modules_hooked = 0, hooked_functions = 0;
    uint64_t contexts_created = 0, contexts_initialized = 0, contexts_destroyed = 0;
    uint64_t executes = 0, replaced = 0, fallbacks = 0, unknown_context = 0;
    char reason[128] = {};
};
Stats GetStats();
void ResetForTests();

} // namespace na::xess
