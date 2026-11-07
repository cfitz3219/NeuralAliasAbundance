// DLSS on the game's own D3D11 device (NGX D3D11). Used by Dx11Bridge's
// InProcess backend. Game-agnostic: it takes resources and a FrameRecord.
//
// Built for real only when the NGX SDK headers and static library are present
// (NA_HAVE_NGX_LIB); otherwise every call reports why it is unavailable.
#pragma once

#include "bridge/frame_record.hpp"

#include <d3d11.h>

#include <memory>

namespace na::bridge {

struct InProcResources {
    ID3D11Resource* color = nullptr;  // render-size region at top-left
    ID3D11Resource* depth = nullptr;
    ID3D11Resource* motion = nullptr;
    ID3D11Resource* output = nullptr; // UAV-capable, output size
};

class Dx11Dlss {
public:
    Dx11Dlss();
    ~Dx11Dlss();
    Dx11Dlss(const Dx11Dlss&) = delete;
    Dx11Dlss& operator=(const Dx11Dlss&) = delete;

    // nullptr when DLSS Super Resolution is ready on this device.
    const char* Init(ID3D11Device* device, const wchar_t* data_path);
    // Records DLSS on `context`. The feature is (re)created when the session
    // size, mode, preset or creation flags change. nullptr on success.
    const char* Evaluate(ID3D11DeviceContext* context, const InProcResources& r, const FrameRecord& f, const SessionLimits& limits);
    void Shutdown();

    static bool BuiltWithDlss();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace na::bridge
