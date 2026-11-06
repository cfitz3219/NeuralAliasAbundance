#pragma once
#include "shader_operation.hpp"
#include <d3d12.h>

namespace na::observe {
enum class TextureKind { ClearColor, ClearDepth, Copy, CopyRegion, Resolve };
// Borrowed native call arguments; valid only through Before/After. A replacement
// records its complete private operation under HostWorkScope. Otherwise the
// original API is forwarded exactly once. No guessed descriptor contents.
struct TextureCall {
    TextureKind kind = TextureKind::Copy;
    uint64_t target_id = 0;
    ID3D12Resource* source = nullptr;
    ID3D12Resource* destination = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE target{};
    float color[4]{}, depth = 0;
    UINT8 stencil = 0;
    D3D12_CLEAR_FLAGS flags{};
    UINT rect_count = 0;
    const D3D12_RECT* rects = nullptr;
    D3D12_TEXTURE_COPY_LOCATION source_location{}, destination_location{};
    const D3D12_BOX* box = nullptr;
    UINT x = 0, y = 0, z = 0, source_sub = 0, destination_sub = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
class TextureOperationHook {
public:
    virtual ~TextureOperationHook() = default;
    virtual ShaderScope Before(ID3D12GraphicsCommandList*, const TextureCall&) noexcept = 0;
    virtual void After(ID3D12GraphicsCommandList*, const TextureCall&, ShaderScope&) noexcept = 0;
};
void SetTextureOperationHook(std::shared_ptr<TextureOperationHook>);
// Independent owners must not replace each other's clear/copy handling. The
// compatibility overload above owns Effects; scene bindings use Bindings.
// Enter in lane order and unwind in reverse, retaining the exact snapshot
// throughout the native call, just like ShaderLane.
enum class TextureLane { Bindings = 0, Effects = 1 };
void SetTextureOperationHook(TextureLane, std::shared_ptr<TextureOperationHook>);
// Observation only: called on the recording thread for each game ClearDepthStencilView that
// clears depth, before it is forwarded, with the view's resource id. Records nothing.
using DepthClearListener = void (*)(ID3D12GraphicsCommandList*, uint64_t target_id);
void SetDepthClearListener(DepthClearListener);
} // namespace na::observe
