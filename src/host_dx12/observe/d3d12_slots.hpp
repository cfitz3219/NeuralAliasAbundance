// C8: vtable slot indices of the D3D12 methods the observation producers hook.
// Every value is checked at compile time against the SDK's C vtable layout in
// d3d12_slots_check.cpp (offsetof in the CINTERFACE structs), so a wrong
// number fails the build instead of hooking the wrong method.
#pragma once

#include <cstdint>

namespace na::observe::slots {

// ID3D12Device .. ID3D12Device8
namespace device {
constexpr uint32_t CreateCommandQueue = 8;
constexpr uint32_t CreateCommandAllocator = 9;
constexpr uint32_t CreateGraphicsPipelineState = 10;
constexpr uint32_t CreateComputePipelineState = 11;
constexpr uint32_t CreateCommandList = 12;
constexpr uint32_t CreateDescriptorHeap = 14;
constexpr uint32_t CreateRootSignature = 16;
constexpr uint32_t CreateConstantBufferView = 17;
constexpr uint32_t CreateShaderResourceView = 18;
constexpr uint32_t CreateUnorderedAccessView = 19;
constexpr uint32_t CreateRenderTargetView = 20;
constexpr uint32_t CreateDepthStencilView = 21;
constexpr uint32_t CreateSampler = 22;
constexpr uint32_t CopyDescriptors = 23;
constexpr uint32_t CopyDescriptorsSimple = 24;
constexpr uint32_t CreateCommittedResource = 27;
constexpr uint32_t CreateHeap = 28;
constexpr uint32_t CreatePlacedResource = 29;
constexpr uint32_t CreateReservedResource = 30;
constexpr uint32_t CreateFence = 36;
constexpr uint32_t CreatePipelineState = 47;       // ID3D12Device2
constexpr uint32_t CreateCommandList1 = 51;        // ID3D12Device4
constexpr uint32_t CreateCommittedResource1 = 53;  // ID3D12Device4
constexpr uint32_t CreateHeap1 = 54;               // ID3D12Device4
constexpr uint32_t CreateReservedResource1 = 55;   // ID3D12Device4
constexpr uint32_t CreateCommittedResource2 = 69;  // ID3D12Device8
constexpr uint32_t CreatePlacedResource1 = 70;     // ID3D12Device8
} // namespace device

namespace queue {
constexpr uint32_t ExecuteCommandLists = 10;
constexpr uint32_t Signal = 14;
constexpr uint32_t Wait = 15;
} // namespace queue

// ID3D12GraphicsCommandList
namespace list {
constexpr uint32_t Close = 9;
constexpr uint32_t Reset = 10;
constexpr uint32_t DrawInstanced = 12;
constexpr uint32_t DrawIndexedInstanced = 13;
constexpr uint32_t Dispatch = 14;
constexpr uint32_t CopyBufferRegion = 15;
constexpr uint32_t CopyTextureRegion = 16;
constexpr uint32_t CopyResource = 17;
constexpr uint32_t ResolveSubresource = 19;
constexpr uint32_t RSSetViewports = 21;
constexpr uint32_t RSSetScissorRects = 22;
constexpr uint32_t SetPipelineState = 25;
constexpr uint32_t ResourceBarrier = 26;
constexpr uint32_t ExecuteBundle = 27;
constexpr uint32_t SetDescriptorHeaps = 28;
constexpr uint32_t SetComputeRootSignature = 29;
constexpr uint32_t SetGraphicsRootSignature = 30;
constexpr uint32_t SetComputeRootDescriptorTable = 31;
constexpr uint32_t SetGraphicsRootDescriptorTable = 32;
constexpr uint32_t SetComputeRoot32BitConstant = 33;
constexpr uint32_t SetGraphicsRoot32BitConstant = 34;
constexpr uint32_t SetComputeRoot32BitConstants = 35;
constexpr uint32_t SetGraphicsRoot32BitConstants = 36;
constexpr uint32_t SetComputeRootConstantBufferView = 37;
constexpr uint32_t SetGraphicsRootConstantBufferView = 38;
constexpr uint32_t SetComputeRootShaderResourceView = 39;
constexpr uint32_t SetGraphicsRootShaderResourceView = 40;
constexpr uint32_t SetComputeRootUnorderedAccessView = 41;
constexpr uint32_t SetGraphicsRootUnorderedAccessView = 42;
constexpr uint32_t OMSetRenderTargets = 46;
constexpr uint32_t ClearDepthStencilView = 47;
constexpr uint32_t ClearRenderTargetView = 48;
constexpr uint32_t ExecuteIndirect = 59;
} // namespace list

} // namespace na::observe::slots
