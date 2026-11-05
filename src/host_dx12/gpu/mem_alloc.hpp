#pragma once
// AMD D3D12 Memory Allocator (MIT, vendored in third_party/d3d12ma, v3.0.1) for NeuralAlias's
// own GPU resources. A resource made here is an ordinary ID3D12Resource placed in memory the
// allocator sub-allocates from shared heaps; the allocation is tied to the resource's own
// lifetime (private data), so callers keep using ComPtr<ID3D12Resource> exactly as with
// CreateCommittedResource, and the memory is freed when the last reference goes.
// Placed render targets / depth targets start uninitialised: only use this where the first
// GPU operation on the resource is a Clear, Discard or full Copy (documented per call site).
// Falls back to CreateCommittedResource whenever the allocator can't serve a request.
#include <d3d12.h>
#include <cstdint>
#include <string>

namespace na::gpu::mem {
HRESULT CreateResource(ID3D12Device* device, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
                       const D3D12_CLEAR_VALUE* clear, ID3D12Resource** out);
struct Stats {
    uint64_t placed = 0, committed_fallbacks = 0, live = 0;
    uint64_t local_usage = 0, local_budget = 0, blocks = 0, block_bytes = 0, allocation_bytes = 0;
    bool available = false;
};
Stats GetStats();
std::string StatusLine();
} // namespace na::gpu::mem
