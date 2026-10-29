#pragma once
#include "host_dx12/nr/fg_composition.hpp"
namespace na::nr {
bool PrepareFgColor(ID3D12Device* device);
// Descriptor storage belongs to one recorded frame. The owner may reuse it
// only after all of that frame's GPU uses have completed.
class FgColorPass {
public:
    void ReleaseInputs() { for (auto& input : refs_) input.Reset(); }
    bool Record(ID3D12GraphicsCommandList* list, bool output_pass,
        CompositionSurface original, ID3D12Resource* nr, ID3D12Resource* sdr,
        ID3D12Resource* output, uint32_t mode, float paper_white);
private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> refs_[4];
};
}
