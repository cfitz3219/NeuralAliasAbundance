#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

namespace na::observe {
struct ShaderDimension {
    uint32_t shader_register = 0, space = 0, offset = 0, bytes = 0, dimension = 0;
    bool integer = false;
    uint32_t visibility = D3D12_SHADER_VISIBILITY_ALL;
};
// Exact creation arguments, carried through the existing descriptor-copy shadow.
// Resource identities remain live-process identities. No game/build profiles.
struct DescriptorDetails {
    uint32_t kind = 0;
    uint64_t resource = 0, counter = 0;
    bool explicit_view = false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
};
struct OperationDescriptor {
    DescriptorDetails view;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource, counter;
    uint64_t version = 0;
};
struct NativeOperation {
    uint64_t list = 0, recording = 0, pipeline_id = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    bool compute = false;
    struct Table { uint32_t param, first, count; };
    std::vector<Table> tables;
    std::vector<OperationDescriptor> descriptors;
    std::vector<OperationDescriptor> colors;
    OperationDescriptor depth;
    std::vector<D3D12_VIEWPORT> viewports;
    std::vector<D3D12_RECT> scissors;
    struct Constant {
        uint32_t param = 0, descriptor = UINT_MAX, bytes = 0;
        uint64_t resource_id = 0, offset = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        std::vector<ShaderDimension> dimensions;
    };
    std::vector<Constant> constants;
};
// Setup only, before the game's descriptors are created. Does not enable scaling.
void SetOperationSnapshots(bool enabled);
// Recording thread, before a native operation. Bounded by the real finite root
// ranges (4096 descriptors), never table-head guessing or event-delivery order.
// False leaves out empty and the game's operation untouched.
bool ListNativeOperation(ID3D12GraphicsCommandList*, bool compute, NativeOperation& out);
bool NativeDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE, OperationDescriptor& out);
} // namespace na::observe
