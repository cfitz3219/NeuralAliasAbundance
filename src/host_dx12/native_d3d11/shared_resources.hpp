// Native D3D11 inputs -> existing D3D12 effects. No D3D11On12 device is used.
// This module owns shared images and the directional GPU handoff only. The
// provider owns scene selection, camera/jitter and the native insertion point.
#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace na::native_d3d11 {

struct ImageDesc {
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool writable12 = false;
};

// Copy validation for prepared, single-sample image planes. It permits array
// slices and mips by selecting ONE exact source/destination subresource. Depth
// must be copied whole with a compatible format, or converted by the provider;
// D24S8 -> R32_FLOAT is not a copy conversion. No guessed resolves/conversions.
bool CompatibleImageCopy(const D3D11_TEXTURE2D_DESC& native, UINT subresource,
                         const ImageDesc& shared) noexcept;

class SharedTexture final {
public:
    SharedTexture() = default;
    SharedTexture(const SharedTexture&) = delete;
    SharedTexture& operator=(const SharedTexture&) = delete;
    SharedTexture(SharedTexture&&) = default;
    SharedTexture& operator=(SharedTexture&&) = delete;
    HRESULT Create(ID3D11Device* native, ID3D12Device* effects, const ImageDesc& desc);
    ID3D11Texture2D* Native() const { return native_.Get(); }
    ID3D12Resource* Effects() const { return effects_.Get(); }
    const ImageDesc& Description() const { return desc_; }
private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> native_;
    Microsoft::WRL::ComPtr<ID3D12Resource> effects_;
    ImageDesc desc_{};
};

// One owner drives the immediate context and effects queue, in submission order.
// The two fences each have exactly one GPU producer; no CPU Signal or wait.
// Every signal below is issued BEFORE a consumer is allowed to enqueue its wait.
// For each frame: native copies -> SignalNative -> WaitEffects -> effects lists
// -> SignalEffects -> WaitNative -> native output copy -> SignalNative(retire).
// Flush only at native signal boundaries, never per draw or descriptor write.
// References to images, allocators and source/destination resources stay with
// the caller's frame slot until BOTH completion values in Retired() are true.
// UINT64_MAX from GetCompletedValue means removal, never successful retirement.
class SharedFences final {
public:
    SharedFences() = default;
    SharedFences(const SharedFences&) = delete;
    SharedFences& operator=(const SharedFences&) = delete;
    HRESULT Create(ID3D11Device* native, ID3D12Device* effects, ID3D12CommandQueue* queue);
    HRESULT SignalNative(uint64_t* value);
    HRESULT WaitEffects(uint64_t native_value);
    HRESULT SignalEffects(uint64_t* value);
    HRESULT WaitNative(uint64_t effects_value);
    bool Retired(uint64_t native_value, uint64_t effects_value) const;
    HRESULT Failure() const { return failure_; }
    ID3D11DeviceContext4* NativeContext() const { return context_.Get(); }
private:
    HRESULT Fail(HRESULT hr);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D11Fence> native11_, effects11_;
    Microsoft::WRL::ComPtr<ID3D12Fence> native12_, effects12_;
    uint64_t native_value_ = 0, effects_value_ = 0;
    HRESULT failure_ = S_OK;
};

// Must also be used before reusing a frame slot, including after failed output
// composition: the earlier producer can still be using the input images.
constexpr bool Completed(uint64_t native_completed, uint64_t effects_completed,
                         uint64_t native_required, uint64_t effects_required) noexcept
{
    return native_completed != UINT64_MAX && effects_completed != UINT64_MAX &&
           native_completed >= native_required && effects_completed >= effects_required;
}

} // namespace na::native_d3d11
