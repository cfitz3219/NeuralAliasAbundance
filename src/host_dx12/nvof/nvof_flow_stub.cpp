// Build-time stub for na::nvof when the NVIDIA Optical Flow SDK headers are
// not available (CMake NA_HAVE_NVOF unset). Keeps every consumer compiling;
// QueryCaps() reports "not available" so live_flow latches itself off with a
// plain-language reason, exactly as it does when the hardware is missing.
// Added 2026-10-23 (C1). The real implementation is nvof_flow.cpp.
#include "host_dx12/nvof/nvof_flow.hpp"

#include <cstdio>
#include <cstring>

namespace na::nvof {
namespace {
constexpr char kReason[] = "optical flow support was not compiled in (NVIDIA Optical Flow SDK headers absent at build time)";
}

bool QueryCaps(ID3D12Device* device, Caps* out, char* error, size_t error_cap)
{
    if (out) *out = Caps{};
    if (error && error_cap) { strncpy_s(error, error_cap, kReason, _TRUNCATE); }
    (void)device;
    return false;
}

Flow::~Flow() { Shutdown(); }

void Flow::set_error(const char* what, int status)
{
    snprintf(error_, sizeof(error_), "%s: %s (status %d)", what, kReason, status);
}

bool Flow::Init(ID3D12Device*, uint32_t width, uint32_t height, uint32_t grid, uint32_t)
{
    width_ = width; height_ = height; grid_ = grid ? grid : 1;
    set_error("Init", 0);
    return false;
}
bool Flow::RegisterInput(ID3D12Resource*, ID3D12Fence*, uint64_t, uint64_t, BufferHandle* out)
{
    if (out) *out = nullptr;
    set_error("RegisterInput", 0);
    return false;
}
bool Flow::RegisterOutput(ID3D12Resource*, ID3D12Fence*, uint64_t, uint64_t, BufferHandle* out)
{
    if (out) *out = nullptr;
    set_error("RegisterOutput", 0);
    return false;
}
bool Flow::Unregister(BufferHandle) { set_error("Unregister", 0); return false; }
bool Flow::Execute(BufferHandle, BufferHandle, BufferHandle, ID3D12Fence*, uint64_t, ID3D12Fence*, uint64_t, bool)
{
    set_error("Execute", 0);
    return false;
}
bool Flow::Shutdown() { return registered_ == 0; }

} // namespace na::nvof
