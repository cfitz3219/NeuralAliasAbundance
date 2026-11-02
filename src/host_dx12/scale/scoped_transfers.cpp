#include "scoped_transfers.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include <algorithm>
namespace na::scale {
namespace {
using Kind = na::observe::TextureKind;
bool edge(UINT v, UINT from, UINT to, UINT& out) {
    if (!from || v > from || (uint64_t(v) * to) % from) return false;
    out = UINT(uint64_t(v) * to / from); return true;
}
bool rect(const D3D12_RECT& r, Extent from, Extent to, D3D12_RECT& out) {
    if(r.left<0 || r.top<0 || r.right<r.left || r.bottom<r.top) return false;
    UINT l,t,rr,b;
    if(!edge(r.left,from.width,to.width,l)||!edge(r.top,from.height,to.height,t)||
       !edge(r.right,from.width,to.width,rr)||!edge(r.bottom,from.height,to.height,b))return false;
    out={LONG(l),LONG(t),LONG(rr),LONG(b)};return true;
}
Extent extent(const D3D12_RESOURCE_DESC& d, UINT sub) {
    const UINT mip=sub%d.MipLevels;
    return {UINT((std::max)(uint64_t(1),d.Width>>mip)),(std::max)(1u,d.Height>>mip)};
}
bool subresource(const Member& member, UINT sub, UINT& mapped) {
    const auto& a=member.native_desc;const auto& b=member.reduced_desc;
    if(sub>=UINT(a.MipLevels)*a.DepthOrArraySize || sub%a.MipLevels>=b.MipLevels)return false;
    mapped=(sub/a.MipLevels)*b.MipLevels+sub%a.MipLevels;return true;
}
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return x;
}
}
std::shared_ptr<const PreparedTransfer> PreparedTransfer::Create(ID3D12Device* dev,TransferSpec s,std::string& error) {
    error.clear();const auto fail=[&](const char* why)->std::shared_ptr<const PreparedTransfer>{error=why;return {};};
    if(!dev||!s.group)return fail("transfer has no scene group");
    if(static_cast<unsigned>(s.kind)>static_cast<unsigned>(Kind::Resolve))return fail("unknown transfer operation");
    const auto* dst=s.group->Find(s.destination);const auto* src=s.group->Find(s.source);
    if(!dst||dst->initial_state!=D3D12_RESOURCE_STATE_COMMON)return fail("transfer destination must be a COMMON group member");
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if(FAILED(dst->reduced->GetDevice(IID_PPV_ARGS(&device)))||device.Get()!=dev)return fail("transfer belongs to a different device");
    const bool clear=s.kind==Kind::ClearColor||s.kind==Kind::ClearDepth;
    if(!clear&&(!src||src==dst||src->initial_state!=D3D12_RESOURCE_STATE_COMMON))return fail("transfer source must be another COMMON group member");
    if(src && (src->native_desc.DepthOrArraySize!=dst->native_desc.DepthOrArraySize ||
       src->native_desc.MipLevels!=dst->native_desc.MipLevels || src->native_desc.Width!=dst->native_desc.Width ||
       src->native_desc.Height!=dst->native_desc.Height || src->reduced_desc.Width!=dst->reduced_desc.Width ||
       src->reduced_desc.Height!=dst->reduced_desc.Height || src->reduced_desc.MipLevels!=dst->reduced_desc.MipLevels))
        return fail("transfer endpoints do not have matching native and reduced extents");
    if(s.kind==Kind::Copy && (src->reduced_desc.Format!=dst->reduced_desc.Format ||
        src->reduced_desc.SampleDesc.Count!=dst->reduced_desc.SampleDesc.Count))return fail("whole-resource copy shapes differ");
    if(s.kind==Kind::Resolve && (src->reduced_desc.SampleDesc.Count<=1 || dst->reduced_desc.SampleDesc.Count!=1 ||
        s.resolve_format==DXGI_FORMAT_UNKNOWN))return fail("resolve requires multisample source and single-sample destination");
    auto p=std::shared_ptr<PreparedTransfer>(new PreparedTransfer);p->spec_=std::move(s);
    if(clear) {
        const bool depth=p->spec_.kind==Kind::ClearDepth;
        if(depth ? !(dst->reduced_desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || p->spec_.depth.Format==DXGI_FORMAT_UNKNOWN
                 : !(dst->reduced_desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || p->spec_.color.Format==DXGI_FORMAT_UNKNOWN)
            return fail("clear needs its actual typed target view");
        const UINT mip=depth?p->spec_.depth.Texture2D.MipSlice:p->spec_.color.Texture2D.MipSlice;
        const bool ms=dst->reduced_desc.SampleDesc.Count>1;
        if(!ms&&mip>=dst->reduced_desc.MipLevels)return fail("clear mip was removed by reduction");
        D3D12_DESCRIPTOR_HEAP_DESC hd{depth?D3D12_DESCRIPTOR_HEAP_TYPE_DSV:D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        if(FAILED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p->clear_))))return fail("private clear view allocation failed");
        if(depth)dev->CreateDepthStencilView(dst->reduced.Get(),&p->spec_.depth,p->clear_->GetCPUDescriptorHandleForHeapStart());
        else dev->CreateRenderTargetView(dst->reduced.Get(),&p->spec_.color,p->clear_->GetCPUDescriptorHandleForHeapStart());
    }
    return p;
}
bool PreparedTransfer::Record(ID3D12GraphicsCommandList* list,const na::observe::TextureCall& c,const char** why)const {
    const auto fail=[&](const char* w){if(why)*why=w;return false;};
    const auto* dst=spec_.group->Find(spec_.destination);const auto* src=spec_.group->Find(spec_.source);
    if(!list||c.kind!=spec_.kind)return fail("transfer operation changed");
    const bool clear=c.kind==Kind::ClearColor||c.kind==Kind::ClearDepth;
    if(clear ? c.target_id!=dst->id : c.destination!=dst->native.Get()||c.source!=src->native.Get())return fail("transfer endpoints changed");
    if(c.rect_count>64 || (c.rect_count&&!c.rects))return fail("clear rectangle list is unavailable");
    D3D12_RECT rects[64]{};
    const UINT clear_mip=c.kind==Kind::ClearDepth?spec_.depth.Texture2D.MipSlice:spec_.color.Texture2D.MipSlice;
    if(clear)for(UINT i=0;i<c.rect_count;++i)if(!rect(c.rects[i],extent(dst->native_desc,clear_mip),extent(dst->reduced_desc,clear_mip),rects[i]))
        return fail("clear rectangle cannot map exactly to the reduced view");
    D3D12_TEXTURE_COPY_LOCATION a=c.destination_location,b=c.source_location;
    D3D12_BOX box{};UINT x=c.x,y=c.y;
    if(c.kind==Kind::CopyRegion) {
        if(a.Type!=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX||b.Type!=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX||c.z)
            return fail("transfer is not a plain 2D subresource copy");
        UINT da,sb;
        if(!subresource(*dst,a.SubresourceIndex,da)||!subresource(*src,b.SubresourceIndex,sb))return fail("copy subresource cannot map");
        const auto nd=extent(dst->native_desc,a.SubresourceIndex),rd=extent(dst->reduced_desc,da);
        const auto ns=extent(src->native_desc,b.SubresourceIndex),rs=extent(src->reduced_desc,sb);
        if(!edge(c.x,nd.width,rd.width,x)||!edge(c.y,nd.height,rd.height,y))return fail("copy offset cannot map");
        if(c.box) {
            if(c.box->front||c.box->back!=1||c.box->left>c.box->right||c.box->top>c.box->bottom||
               !edge(c.box->left,ns.width,rs.width,box.left)||!edge(c.box->right,ns.width,rs.width,box.right)||
               !edge(c.box->top,ns.height,rs.height,box.top)||!edge(c.box->bottom,ns.height,rs.height,box.bottom))return fail("copy box cannot map exactly");
            box.back=1;
        }
        const UINT bw=c.box?box.right-box.left:rs.width,bh=c.box?box.bottom-box.top:rs.height;
        if(x+bw>rd.width||y+bh>rd.height)return fail("reduced copy exceeds destination");
        a.pResource=dst->reduced.Get();b.pResource=src->reduced.Get();a.SubresourceIndex=da;b.SubresourceIndex=sb;
    }
    UINT resolve_src=0,resolve_dst=0;
    if(c.kind==Kind::Resolve&&(c.format!=spec_.resolve_format || !subresource(*src,c.source_sub,resolve_src)||
       !subresource(*dst,c.destination_sub,resolve_dst)))return fail("resolve view changed");
    auto owner=std::const_pointer_cast<PreparedTransfer>(shared_from_this());
    if(!na::nr::native::RetainRecording(list,owner))return fail("transfer recording lifetime unavailable");
    auto target=c.kind==Kind::ClearDepth?D3D12_RESOURCE_STATE_DEPTH_WRITE:c.kind==Kind::ClearColor?D3D12_RESOURCE_STATE_RENDER_TARGET:
        c.kind==Kind::Resolve?D3D12_RESOURCE_STATE_RESOLVE_DEST:D3D12_RESOURCE_STATE_COPY_DEST;
    auto input=c.kind==Kind::Resolve?D3D12_RESOURCE_STATE_RESOLVE_SOURCE:D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto db=barrier(dst->reduced.Get(),D3D12_RESOURCE_STATE_COMMON,target);list->ResourceBarrier(1,&db);
    if(!clear){auto sb=barrier(src->reduced.Get(),D3D12_RESOURCE_STATE_COMMON,input);list->ResourceBarrier(1,&sb);}
    switch(c.kind) {
    case Kind::ClearColor:list->ClearRenderTargetView(clear_->GetCPUDescriptorHandleForHeapStart(),c.color,c.rect_count,c.rect_count?rects:nullptr);break;
    case Kind::ClearDepth:list->ClearDepthStencilView(clear_->GetCPUDescriptorHandleForHeapStart(),c.flags,c.depth,c.stencil,c.rect_count,c.rect_count?rects:nullptr);break;
    case Kind::Copy:list->CopyResource(dst->reduced.Get(),src->reduced.Get());break;
    case Kind::CopyRegion:list->CopyTextureRegion(&a,x,y,0,&b,c.box?&box:nullptr);break;
    case Kind::Resolve:list->ResolveSubresource(dst->reduced.Get(),resolve_dst,src->reduced.Get(),resolve_src,c.format);break;
    }
    if(!clear){auto sb=barrier(src->reduced.Get(),input,D3D12_RESOURCE_STATE_COMMON);list->ResourceBarrier(1,&sb);}
    std::swap(db.Transition.StateBefore,db.Transition.StateAfter);list->ResourceBarrier(1,&db);return true;
}
na::observe::ShaderScope TransferHook::Before(ID3D12GraphicsCommandList* list,const na::observe::TextureCall& c)noexcept {
    const auto p=provider_?provider_->Acquire(list,c):nullptr;if(!p)return {};
    const char* why="transfer unavailable";
    if(!p->Record(list,c,&why)){provider_->Refused(list,c,why);return {};}
    return {na::observe::ShaderAction::Replaced,std::const_pointer_cast<PreparedTransfer>(p)};
}
} // namespace na::scale
