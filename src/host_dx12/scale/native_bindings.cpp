#include "native_bindings.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/observe/producers.hpp"
#include <algorithm>
namespace na::scale {
bool NativeBindingSpec(const na::observe::NativeOperation& op, std::shared_ptr<const SceneGroup> group,
                       BindingSpec& out, std::string& error)
{
    out = {}; error.clear();
    const auto fail = [&](const char* why) { error = why; return false; };
    if (!group || !op.root || op.compute) return fail("native raster bindings are required");
    BindingSpec s; s.group = group; s.root = op.root; s.native_extent = group->Output(); s.render_extent = group->Render();
    const auto use = [&](uint64_t member, D3D12_RESOURCE_STATES state, UINT sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        for (auto& u : s.uses) if (u.member == member && u.subresource == sub) { u.state |= state; return; }
        s.uses.push_back({member, sub, state});
    };
    for (const auto& t : op.tables) s.tables.push_back({t.param,t.first});
    for (const auto& d : op.descriptors) {
        ShaderView v; const auto* member = group->Find(d.view.resource);
        v.resource = member ? member->reduced : d.resource; v.counter = d.counter; v.default_view = !d.view.explicit_view;
        if (d.view.kind == NA_DESC_CBV) { v.kind = ShaderView::Kind::NativeCbv; v.cbv = d.view.cbv; }
        else if (d.view.kind == NA_DESC_SRV) {
            v.kind = ShaderView::Kind::Srv; v.srv = d.view.srv;
            if (member) use(member->id, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                member->role == Role::Depth && v.srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D ? v.srv.Texture2D.PlaneSlice : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        } else if (d.view.kind == NA_DESC_UAV) {
            v.kind = ShaderView::Kind::Uav; v.uav = d.view.uav;
            if (member) use(member->id, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else return fail("resource table contains an unrecognized descriptor");
        s.views.push_back(std::move(v));
    }
    for (const auto& d : op.colors) {
        const auto* member = group->Find(d.view.resource);
        if (!member) return fail("all coupled render targets must belong to the scene group");
        auto view = d.view.rtv;
        if (!d.view.explicit_view) { view.Format = member->reduced_desc.Format; view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; }
        s.colors.push_back({member->id,view}); use(member->id,D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (op.depth.view.resource) {
        const auto* member = group->Find(op.depth.view.resource);
        if (!member) return fail("the coupled depth target must belong to the scene group");
        auto view = op.depth.view.dsv;
        if (!op.depth.view.explicit_view) {
            view.Format = member->reduced_desc.Format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_D32_FLOAT : member->reduced_desc.Format;
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        }
        s.depth = {member->id,view};
        use(member->id,view.Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE,0);
        if (view.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || view.Format == DXGI_FORMAT_D24_UNORM_S8_UINT)
            use(member->id,view.Flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE,1);
    }
    for (const auto& u : s.uses) {
        const UINT write = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if ((u.state & write) && (u.state & ~write)) return fail("one operation requires conflicting read/write states");
    }
    for(const auto& c:op.constants) {
        std::vector<uint8_t> bytes(c.bytes);
        if(na::observe::ReadBuffer(c.resource_id,c.offset,bytes.data(),c.bytes)!=c.bytes)return fail("reflected scene dimensions are not CPU-visible at preparation");
        std::vector<ConstantPatch> patches;
        for(const auto& field:c.dimensions)patches.push_back({field.offset,field.integer?Scalar::UInt:Scalar::Float,
            static_cast<Dimension>(field.dimension),s.native_extent,s.render_extent});
        Microsoft::WRL::ComPtr<ID3D12Device> device;if(!c.resource||FAILED(c.resource->GetDevice(IID_PPV_ARGS(&device))))return fail("constant-buffer device is unavailable");
        auto constants=GpuConstants::Create(device.Get(),c.resource.Get(),c.offset,bytes.data(),bytes.size(),patches,error);if(!constants)return false;
        D3D12_HEAP_PROPERTIES heap{};if(FAILED(c.resource->GetHeapProperties(&heap,nullptr)))return fail("constant-buffer heap is unavailable");
        D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_GENERIC_READ;
        if(heap.Type!=D3D12_HEAP_TYPE_UPLOAD&&heap.Type!=static_cast<D3D12_HEAP_TYPE>(5)) {
            uint32_t actual=0;if(!na::observe::KnownSubStateNow(c.resource_id,0,&actual))return fail("GPU constant-buffer state is not yet established");
            state=static_cast<D3D12_RESOURCE_STATES>(actual);
        }
        const uint32_t index=static_cast<uint32_t>(s.constants.size());s.constants.push_back({std::move(constants),state});
        if(c.descriptor==UINT_MAX)s.root_cbvs.push_back({c.param,index});
        else if(c.descriptor<s.views.size()){s.views[c.descriptor].kind=ShaderView::Kind::PatchedCbv;s.views[c.descriptor].constant=index;}
        else return fail("reflected descriptor is outside its table");
    }
    s.viewports = op.viewports; s.scissors = op.scissors;
    out = std::move(s); return true;
}
}
