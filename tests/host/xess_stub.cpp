// Stand-in for a game's libxess.dll (D3D12 entry points) for the C7b test.
// Uses OUR ABI declarations (Intel's headers are not vendored); records what
// reached it so the test can tell whether XeSS ran or was replaced by DLSS.
#include "host_dx12/upscalers/xess_abi.hpp"

using namespace na::xess::abi;

namespace {
int g_creates = 0, g_inits = 0, g_executes = 0, g_destroys = 0, g_scales = 0;
uintptr_t g_next = 0x9000;
} // namespace

extern "C" {
__declspec(dllexport) Result xessD3D12CreateContext(ID3D12Device* device, Context* out)
{
    if (!device || !out) return kErrorInvalidArgument;
    ++g_creates;
    *out = reinterpret_cast<Context>(g_next);
    g_next += 0x10;
    return kSuccess;
}
__declspec(dllexport) Result xessD3D12Init(Context c, const D3D12InitParams* p)
{
    if (!c || !p) return kErrorInvalidArgument;
    g_inits += p->qualitySetting >= kUltraPerformance ? 1 : 2;
    return kSuccess;
}
__declspec(dllexport) Result xessD3D12Execute(Context c, ID3D12GraphicsCommandList* list, const D3D12ExecuteParams* p)
{
    if (!c || !list || !p) return kErrorInvalidArgument;
    ++g_executes;
    return kSuccess;
}
__declspec(dllexport) Result xessDestroyContext(Context c)
{
    if (!c) return kErrorInvalidContext;
    ++g_destroys;
    return kSuccess;
}
__declspec(dllexport) Result xessSetJitterScale(Context c, float x, float y)
{
    if (!c) return kErrorInvalidContext;
    g_scales += (x != 0.f) + (y != 0.f) + 1;
    return kSuccess;
}
__declspec(dllexport) Result xessSetVelocityScale(Context c, float x, float y)
{
    if (!c) return kErrorInvalidContext;
    g_scales += (x != 1.f) + (y != 1.f) + 3;
    return kSuccess;
}
__declspec(dllexport) Result xessSetExposureMultiplier(Context c, float m)
{
    if (!c) return kErrorInvalidContext;
    g_scales += m > 1.f ? 5 : 7;
    return kSuccess;
}
__declspec(dllexport) int XessStubExecutes() { return g_executes; }
__declspec(dllexport) int XessStubCreates() { return g_creates + g_destroys * 1000; }
}
