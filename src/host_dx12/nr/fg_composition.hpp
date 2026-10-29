#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace na::nr {
struct CompositionSurface {
    ID3D12Resource* texture = nullptr;
    uint32_t x = 0, y = 0;
};
enum class UiComposition : uint32_t { EqualPixels, AlphaRed, UiAlpha };
// Call during device/NR preparation, never from native FG evaluation.
bool PrepareFgComposition(ID3D12Device* device);

// One immutable recording at a time; reuse through TryRecycle after completion.
// The caller retains this through command-list
// submission AND GPU completion (or a confirmed discarded recording). No waits
// or game submissions here. Inputs must be in NON_PIXEL_SHADER_RESOURCE and
// enhanced must already be in the SAME composition colour domain as
// full/hudless. This pass does not infer PQ versus linear UI blending or
// perform tone mapping; the caller must establish the SDK's B=UI+(1-a)H
// relation in that domain. GPU tests cover linear HDR, not arbitrary PQ UI.
// Output regions are compact, in NON_PIXEL_SHADER_RESOURCE after Record.
// The caller copies only those regions back, preserving allocation padding.
class FgComposition {
public:
    FgComposition() = default;
    FgComposition(const FgComposition&) = delete;
    FgComposition& operator=(const FgComposition&) = delete;
    bool Record(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height,
                CompositionSurface full, CompositionSurface hudless,
                CompositionSurface enhanced, CompositionSurface alpha,
                UiComposition mode);
    // Associate the actual final consuming submission. A later use on the
    // same fence may extend retirement; different fences require an owner
    // which joins their completion before supplying this final receipt.
    bool SetCompletion(ID3D12Fence* fence, uint64_t value);
    bool TryRecycle(); // nonblocking; never accepts an unsubmitted recording
    ID3D12Resource* Full() const { return full_.Get(); }
    ID3D12Resource* Hudless() const { return hudless_.Get(); }
private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> inputs_[4], full_, hudless_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Fence> completion_;
    uint64_t completion_value_ = 0;
    bool outputs_readable_ = false;
    bool recorded_ = false;
};
} // namespace na::nr
