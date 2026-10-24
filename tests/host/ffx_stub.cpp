// Stand-in for a game's FidelityFX API runtime (amd_fidelityfx_dx12.dll) for
// the C7 interception test. Exports the five ffx* entry points with the real
// signatures and records what reached it, so the test can tell whether FSR
// ran (observe / fallback) or was skipped (replaced by DLSS).
#include <Kits/FidelityFX/api/include/ffx_api.h>
#include <Kits/FidelityFX/upscalers/include/ffx_upscale.h>

#include <cstdint>

namespace {
int g_creates = 0, g_destroys = 0, g_dispatches = 0, g_upscale_dispatches = 0;
uintptr_t g_next = 0x7000;
float g_last_jitter_x = 0;
} // namespace

extern "C" {
FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks*)
{
    if (!context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    ++g_creates;
    *context = reinterpret_cast<ffxContext>(g_next);
    g_next += 0x10;
    return FFX_API_RETURN_OK;
}
FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks*)
{
    if (!context) return FFX_API_RETURN_ERROR_PARAMETER;
    ++g_destroys;
    *context = nullptr;
    return FFX_API_RETURN_OK;
}
FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext*, const ffxConfigureDescHeader*) { return FFX_API_RETURN_OK; }
FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext*, ffxQueryDescHeader*) { return FFX_API_RETURN_OK; }
FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    if (!context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    ++g_dispatches;
    if (desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        ++g_upscale_dispatches;
        g_last_jitter_x = reinterpret_cast<const ffxDispatchDescUpscale*>(desc)->jitterOffset.x;
    }
    return FFX_API_RETURN_OK;
}

__declspec(dllexport) int StubCounts(int which)
{
    switch (which) {
    case 0: return g_creates;
    case 1: return g_destroys;
    case 2: return g_dispatches;
    default: return g_upscale_dispatches;
    }
}
__declspec(dllexport) float StubLastJitterX() { return g_last_jitter_x; }
}
