// Builds without the NVIDIA DLSS SDK headers: in-process DX11 DLSS reports
// itself unavailable, and the bridge uses the helper instead.
#include "bridge/dx11_dlss_inproc.hpp"

namespace na::bridge {

struct Dx11Dlss::Impl {};

Dx11Dlss::Dx11Dlss() : impl_(std::make_unique<Impl>()) {}
Dx11Dlss::~Dx11Dlss() = default;
bool Dx11Dlss::BuiltWithDlss() { return false; }
const char* Dx11Dlss::Init(ID3D11Device*, const wchar_t*)
{
    return "This build has no NVIDIA DLSS SDK, so DLSS cannot run inside a DX11 game.";
}
const char* Dx11Dlss::Evaluate(ID3D11DeviceContext*, const InProcResources&, const FrameRecord&, const SessionLimits&)
{
    return "This build has no NVIDIA DLSS SDK.";
}
void Dx11Dlss::Shutdown() {}

} // namespace na::bridge
