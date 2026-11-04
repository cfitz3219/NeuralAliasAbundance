#pragma once
#include "core/jitter_domain.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace na::jitter::gpu {
// Reads the caller's fixed-colour-transform image pair on the raw source grid.
// Known encode offsets can be removed from the hypotheses; the production
// caller supplies zero because it keeps this pair before jitter resampling.
// Slot reuse and Collect require the caller's submitted RecordingLease to
// retire. This class never waits for a GPU fence or owns a second queue.
class Probe {
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> counts, readback;
        Source source{}; uint64_t frame=0; bool recorded=false;
    };
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12Resource> zero_;
    std::vector<Slot> slots_;
    uint32_t stride_=0;
    bool Prepare(Slot&);
public:
    bool Init(ID3D12Device*);
    // Inputs are NPSR; both deltas are CURRENT minus PREVIOUS, in pixels.
    // raster_delta: raw J1 write sample. encode_delta: offsets used by the
    // image encoder. These must remain separate even after source calibration.
    bool Record(ID3D12GraphicsCommandList*,uint32_t slot,ID3D12Resource* current,ID3D12Resource* previous,
                Source,uint64_t frame,Sample raster_delta,Sample encode_delta);
    bool Collect(uint32_t slot,Source&,Evidence&); // completed slot only; no wait
    void Discard(uint32_t slot) { if(slot<slots_.size())slots_[slot].recorded=false; }
};
}
