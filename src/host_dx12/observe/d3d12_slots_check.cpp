// C8: compile-time proof of every slot index in d3d12_slots.hpp, using the C
// vtable structs the SDK header defines under CINTERFACE.
#define CINTERFACE
#define COBJMACROS
#include <d3d12.h>

#include <cstddef>

#include "host_dx12/observe/d3d12_slots.hpp"

#define NA_SLOT(vtbl, method, value) \
    static_assert(offsetof(vtbl, method) / sizeof(void*) == (value), #vtbl "::" #method " slot")

namespace s = na::observe::slots;
NA_SLOT(ID3D12DeviceVtbl, CreateCommandQueue, s::device::CreateCommandQueue);
NA_SLOT(ID3D12DeviceVtbl, CreateCommandAllocator, s::device::CreateCommandAllocator);
NA_SLOT(ID3D12DeviceVtbl, CreateGraphicsPipelineState, s::device::CreateGraphicsPipelineState);
NA_SLOT(ID3D12DeviceVtbl, CreateComputePipelineState, s::device::CreateComputePipelineState);
NA_SLOT(ID3D12DeviceVtbl, CreateCommandList, s::device::CreateCommandList);
NA_SLOT(ID3D12DeviceVtbl, CreateDescriptorHeap, s::device::CreateDescriptorHeap);
NA_SLOT(ID3D12DeviceVtbl, CreateRootSignature, s::device::CreateRootSignature);
NA_SLOT(ID3D12DeviceVtbl, CreateConstantBufferView, s::device::CreateConstantBufferView);
NA_SLOT(ID3D12DeviceVtbl, CreateShaderResourceView, s::device::CreateShaderResourceView);
NA_SLOT(ID3D12DeviceVtbl, CreateUnorderedAccessView, s::device::CreateUnorderedAccessView);
NA_SLOT(ID3D12DeviceVtbl, CreateRenderTargetView, s::device::CreateRenderTargetView);
NA_SLOT(ID3D12DeviceVtbl, CreateDepthStencilView, s::device::CreateDepthStencilView);
NA_SLOT(ID3D12DeviceVtbl, CreateSampler, s::device::CreateSampler);
NA_SLOT(ID3D12DeviceVtbl, CopyDescriptors, s::device::CopyDescriptors);
NA_SLOT(ID3D12DeviceVtbl, CopyDescriptorsSimple, s::device::CopyDescriptorsSimple);
NA_SLOT(ID3D12DeviceVtbl, CreateCommittedResource, s::device::CreateCommittedResource);
NA_SLOT(ID3D12DeviceVtbl, CreateHeap, s::device::CreateHeap);
NA_SLOT(ID3D12DeviceVtbl, CreatePlacedResource, s::device::CreatePlacedResource);
NA_SLOT(ID3D12DeviceVtbl, CreateReservedResource, s::device::CreateReservedResource);
NA_SLOT(ID3D12DeviceVtbl, CreateFence, s::device::CreateFence);
NA_SLOT(ID3D12Device2Vtbl, CreatePipelineState, s::device::CreatePipelineState);
NA_SLOT(ID3D12Device4Vtbl, CreateCommandList1, s::device::CreateCommandList1);
NA_SLOT(ID3D12Device4Vtbl, CreateCommittedResource1, s::device::CreateCommittedResource1);
NA_SLOT(ID3D12Device4Vtbl, CreateHeap1, s::device::CreateHeap1);
NA_SLOT(ID3D12Device4Vtbl, CreateReservedResource1, s::device::CreateReservedResource1);
NA_SLOT(ID3D12Device8Vtbl, CreateCommittedResource2, s::device::CreateCommittedResource2);
NA_SLOT(ID3D12Device8Vtbl, CreatePlacedResource1, s::device::CreatePlacedResource1);

NA_SLOT(ID3D12ResourceVtbl, Map, s::resource::Map);
NA_SLOT(ID3D12ResourceVtbl, Unmap, s::resource::Unmap);
NA_SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists, s::queue::ExecuteCommandLists);
NA_SLOT(ID3D12CommandQueueVtbl, Signal, s::queue::Signal);
NA_SLOT(ID3D12CommandQueueVtbl, Wait, s::queue::Wait);

NA_SLOT(ID3D12GraphicsCommandListVtbl, Close, s::list::Close);
NA_SLOT(ID3D12GraphicsCommandListVtbl, Reset, s::list::Reset);
NA_SLOT(ID3D12GraphicsCommandListVtbl, DrawInstanced, s::list::DrawInstanced);
NA_SLOT(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced, s::list::DrawIndexedInstanced);
NA_SLOT(ID3D12GraphicsCommandListVtbl, Dispatch, s::list::Dispatch);
NA_SLOT(ID3D12GraphicsCommandListVtbl, CopyBufferRegion, s::list::CopyBufferRegion);
NA_SLOT(ID3D12GraphicsCommandListVtbl, CopyTextureRegion, s::list::CopyTextureRegion);
NA_SLOT(ID3D12GraphicsCommandListVtbl, CopyResource, s::list::CopyResource);
NA_SLOT(ID3D12GraphicsCommandListVtbl, ResolveSubresource, s::list::ResolveSubresource);
NA_SLOT(ID3D12GraphicsCommandListVtbl, RSSetViewports, s::list::RSSetViewports);
NA_SLOT(ID3D12GraphicsCommandListVtbl, RSSetScissorRects, s::list::RSSetScissorRects);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetPipelineState, s::list::SetPipelineState);
NA_SLOT(ID3D12GraphicsCommandListVtbl, ResourceBarrier, s::list::ResourceBarrier);
NA_SLOT(ID3D12GraphicsCommandListVtbl, ExecuteBundle, s::list::ExecuteBundle);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetDescriptorHeaps, s::list::SetDescriptorHeaps);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootSignature, s::list::SetComputeRootSignature);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootSignature, s::list::SetGraphicsRootSignature);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootDescriptorTable, s::list::SetComputeRootDescriptorTable);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable, s::list::SetGraphicsRootDescriptorTable);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstant, s::list::SetComputeRoot32BitConstant);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRoot32BitConstant, s::list::SetGraphicsRoot32BitConstant);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstants, s::list::SetComputeRoot32BitConstants);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRoot32BitConstants, s::list::SetGraphicsRoot32BitConstants);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootConstantBufferView, s::list::SetComputeRootConstantBufferView);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootConstantBufferView, s::list::SetGraphicsRootConstantBufferView);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootShaderResourceView, s::list::SetComputeRootShaderResourceView);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootShaderResourceView, s::list::SetGraphicsRootShaderResourceView);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootUnorderedAccessView, s::list::SetComputeRootUnorderedAccessView);
NA_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootUnorderedAccessView, s::list::SetGraphicsRootUnorderedAccessView);
NA_SLOT(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets, s::list::OMSetRenderTargets);
NA_SLOT(ID3D12GraphicsCommandListVtbl, ClearDepthStencilView, s::list::ClearDepthStencilView);
NA_SLOT(ID3D12GraphicsCommandListVtbl, ClearRenderTargetView, s::list::ClearRenderTargetView);
NA_SLOT(ID3D12GraphicsCommandListVtbl, ExecuteIndirect, s::list::ExecuteIndirect);
