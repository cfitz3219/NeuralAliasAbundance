#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstddef>
#include <vector>
#include "host_dx12/host_observe.h"

namespace na::observe {
struct ShaderDimension {
    uint32_t shader_register = 0, space = 0, offset = 0, bytes = 0, dimension = 0;
    bool integer = false;
    uint32_t visibility = D3D12_SHADER_VISIBILITY_ALL;
    bool optional = false; // a named size vector's trailing pair may carry inverse size or unrelated padding
};
struct ShaderResource {
    uint32_t type=0, first=0, count=0, space=0, visibility=D3D12_SHADER_VISIBILITY_ALL;
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
    bool used = true; // false only with complete standard shader reflection
};
struct NativeOperation {
    uint64_t list = 0, recording = 0, pipeline_id = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    bool compute = false;
    bool reflection_complete = false;
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
// Components are shared by the full worker request and allocation-free live
// key. Material-only tables remain native; their identities are irrelevant to
// a plan that changes only raster targets and named root dimension buffers.
struct OperationKey {
    uint64_t state=1469598103934665603ull, descriptors=1469598103934665603ull, constants=1469598103934665603ull;
    static void bytes(uint64_t& h,const void* data,size_t count){const auto* p=static_cast<const unsigned char*>(data);while(count--){h^=*p++;h*=1099511628211ull;}}
    template<class T>static void add(uint64_t& h,const T& value){bytes(h,&value,sizeof value);}
    void descriptor(const DescriptorDetails& d,bool used){add(descriptors,used);if(!used)return;
        add(descriptors,d.kind);add(descriptors,d.resource);add(descriptors,d.counter);add(descriptors,d.explicit_view);
        if(d.kind==NA_DESC_CBV)add(descriptors,d.cbv);else if(d.kind==NA_DESC_SRV)add(descriptors,d.srv);else add(descriptors,d.uav);}
    void constant(uint32_t param,uint32_t descriptor,uint64_t resource,uint64_t offset,const ShaderDimension& field){
        uint64_t h=1469598103934665603ull;add(h,param);add(h,descriptor);add(h,resource);add(h,offset);add(h,field.offset);add(h,field.dimension);add(h,field.visibility);constants^=h;}
    uint64_t finish(bool rebase){add(state,constants);add(state,rebase);if(rebase)add(state,descriptors);return state;}
};
struct NativeOperationKey {
    uint64_t key=0,list=0,recording=0;
};
// Does not retain resources, allocate descriptor snapshots, or call GetDesc.
// Reads existing per-list shadow plus immutable PSO/compiler metadata only.
// members must be sorted unique IDs (the automatic owner's canonical group).
bool ListNativeOperationKey(ID3D12GraphicsCommandList*,bool compute,const uint64_t* members,size_t member_count,NativeOperationKey&);
// Setup only, before the game's descriptors are created. Does not enable scaling.
void SetOperationSnapshots(bool enabled);
// Recording thread, before a native operation. Bounded by the real finite root
// ranges (4096 descriptors), never table-head guessing or event-delivery order.
// False leaves out empty and the game's operation untouched.
bool ListNativeOperation(ID3D12GraphicsCommandList*, bool compute, NativeOperation& out,const char** reason=nullptr);
bool NativeDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE, OperationDescriptor& out);
} // namespace na::observe
