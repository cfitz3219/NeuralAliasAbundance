#include "test_support.hpp"
#include "host_dx12/scale/scoped_transfers.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include <d3d12sdklayers.h>
using namespace na::scale;
using Kind=na::observe::TextureKind;
namespace {
void __cdecl sink(const na_event_batch*,void*){}
class Provider final:public TransferProvider {
public:
    ID3D12GraphicsCommandList* list=nullptr;
    uint64_t a=0,b=0,depth=0;
    std::shared_ptr<const PreparedTransfer> clear_a,clear_b,clear_depth,copy,region;
    uint64_t multisample=0;
    std::shared_ptr<const PreparedTransfer> clear_ms,resolve;
    unsigned refused=0;
    std::shared_ptr<const PreparedTransfer> Acquire(ID3D12GraphicsCommandList* l,const na::observe::TextureCall& c)noexcept override {
        if(l!=list)return {};
        if(c.kind==Kind::ClearColor)return c.target_id==a?clear_a:c.target_id==b?clear_b:c.target_id==multisample?clear_ms:nullptr;
        if(c.kind==Kind::ClearDepth&&c.target_id==depth)return clear_depth;
        if(c.kind==Kind::Copy)return copy;
        if(c.kind==Kind::CopyRegion)return region;
        if(c.kind==Kind::Resolve)return resolve;
        return {};
    }
    void Refused(ID3D12GraphicsCommandList*,const na::observe::TextureCall&,const char* why)noexcept override {
        ++refused;std::printf("refused: %s\n",why);
    }
};
struct Readback {
    test::ComPtr<ID3D12Resource> data;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    void Prepare(ID3D12Device* d,ID3D12Resource* r) {
        auto desc=r->GetDesc();UINT64 bytes=0;d->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
        data=test::CreateBuffer(d,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void Record(ID3D12GraphicsCommandList* l,ID3D12Resource* r) {
        auto b=test::Transition(r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);l->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION s{},d{};s.pResource=r;s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.pResource=data.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.PlacedFootprint=fp;l->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
    }
};
}
int main(){
    test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("scoped_transfers_test");
    auto* dev=d.device.Get();test::ComPtr<ID3D12InfoQueue> messages;dev->QueryInterface(IID_PPV_ARGS(&messages));
    REQUIRE(na::observe::InstallProducers(dev,d.queue.Get(),false));na_observe_config cfg{};cfg.size_bytes=sizeof cfg;
    REQUIRE(na_observe_start(&cfg,sink,nullptr)==NA_OK);
    REQUIRE(na::observe::InstallListResetTracking(dev,&na::nr::native::Reset));
    constexpr UINT w=32,h=16,rw=16,rh=8;const auto fmt=DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto a=test::CreateTexture(dev,w,h,fmt,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto b=test::CreateTexture(dev,w,h,fmt,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto z=test::CreateTexture(dev,w,h,DXGI_FORMAT_D32_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(a&&b&&z);const auto aid=na::observe::IdOf(a.Get()),bid=na::observe::IdOf(b.Get()),zid=na::observe::IdOf(z.Get());
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    test::ComPtr<ID3D12DescriptorHeap> rt,ds;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rt))));
    hd={D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&ds))));
    auto av=rt->GetCPUDescriptorHandleForHeapStart(),bv=av;bv.ptr+=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto zv=ds->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(a.Get(),nullptr,av);dev->CreateRenderTargetView(b.Get(),nullptr,bv);dev->CreateDepthStencilView(z.Get(),nullptr,zv);
    auto p=std::make_shared<Provider>();p->a=aid;p->b=bid;p->depth=zid;std::shared_ptr<const SceneGroup> group;std::string error;
    {
        na::observe::HostWorkScope host;
        group=SceneGroup::Create(dev,1,{w,h},{rw,rh},{{aid,a.Get(),Role::Color,{rw,rh}},{bid,b.Get(),Role::History,{rw,rh}},{zid,z.Get(),Role::Depth,{rw,rh}}},error);REQUIRE(group);
        TransferSpec s;s.group=group;s.kind=Kind::ClearColor;s.destination=aid;s.color.Format=fmt;s.color.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        p->clear_a=PreparedTransfer::Create(dev,s,error);s.destination=bid;p->clear_b=PreparedTransfer::Create(dev,s,error);
        s.kind=Kind::ClearDepth;s.destination=zid;s.depth.Format=DXGI_FORMAT_D32_FLOAT;s.depth.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        p->clear_depth=PreparedTransfer::Create(dev,s,error);
        s.kind=Kind::Copy;s.destination=bid;s.source=aid;p->copy=PreparedTransfer::Create(dev,s,error);
        s.kind=Kind::CopyRegion;p->region=PreparedTransfer::Create(dev,s,error);
        REQUIRE(p->clear_a&&p->clear_b&&p->clear_depth&&p->copy&&p->region);
    }
    auto hook=std::make_shared<TransferHook>(p);na::observe::SetTextureOperationHook(hook);
    test::List recording;REQUIRE(recording.Create(dev));REQUIRE(SUCCEEDED(recording.list->Close()));recording.Reset();auto* l=recording.list.Get();p->list=l;
    const float red[4]={3,2,.5f,.25f},blue[4]={0,0,4,1},green[4]={0,1,0,.75f};
    l->ClearRenderTargetView(av,red,0,nullptr);l->ClearRenderTargetView(bv,blue,0,nullptr);
    l->CopyResource(b.Get(),a.Get());l->ClearRenderTargetView(av,green,0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=a.Get();dst.pResource=b.Get();src.Type=dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0,0,0,16,8,1};l->CopyTextureRegion(&dst,16,8,0,&src,&box);
    l->ClearDepthStencilView(zv,D3D12_CLEAR_FLAG_DEPTH,.375f,0,0,nullptr);
    CHECK(p->refused==0);
    // Fractional copy edges must be refused before private commands. The owning
    // route will promote before forwarding; do not submit an invalid toy native copy.
    {na::observe::HostWorkScope host;na::observe::TextureCall c;c.kind=Kind::CopyRegion;c.source=a.Get();c.destination=b.Get();
        c.source_location=src;c.destination_location=dst;D3D12_BOX odd{1,0,0,16,8,1};c.box=&odd;const char* why=nullptr;
        CHECK(!p->region->Record(l,c,&why));CHECK(why!=nullptr);}
    Readback color,depth; color.Prepare(dev,group->Find(bid)->reduced.Get());depth.Prepare(dev,group->Find(zid)->reduced.Get());
    {na::observe::HostWorkScope host;color.Record(l,group->Find(bid)->reduced.Get());depth.Record(l,group->Find(zid)->reduced.Get());}
    REQUIRE(SUCCEEDED(l->Close()));ID3D12CommandList* lists[]{l};
    {na::nr::native::Submission own(d.queue.Get(),1,lists);d.queue->ExecuteCommandLists(1,lists);}d.WaitIdle();
    void* bytes=nullptr;REQUIRE(SUCCEEDED(color.data->Map(0,nullptr,&bytes)));unsigned wrong=0;
    for(UINT y=0;y<rh;++y)for(UINT x=0;x<rw;++x){float pixel[4];std::memcpy(pixel,static_cast<char*>(bytes)+color.fp.Offset+y*color.fp.Footprint.RowPitch+x*16,16);
        const auto* expected=x>=8&&y>=4?green:red;for(int k=0;k<4;++k)wrong+=pixel[k]!=expected[k];}
    color.data->Unmap(0,nullptr);CHECK(wrong==0);std::printf("reduced clear/history copy/boxed copy wrong channels %u\n",wrong);
    REQUIRE(SUCCEEDED(depth.data->Map(0,nullptr,&bytes)));wrong=0;
    for(UINT y=0;y<rh;++y)for(UINT x=0;x<rw;++x){float v;std::memcpy(&v,static_cast<char*>(bytes)+depth.fp.Offset+y*depth.fp.Footprint.RowPitch+x*4,4);wrong+=v!=.375f;}
    depth.data->Unmap(0,nullptr);CHECK(wrong==0);
    std::weak_ptr<const SceneGroup> weak=group;
    group.reset();p->clear_a.reset();p->clear_b.reset();p->clear_depth.reset();p->copy.reset();p->region.reset();
    CHECK(!weak.expired()); // GPU completion alone cannot retire executable descriptors/images
    recording.Reset();na::nr::native::Maintain();CHECK(weak.expired());
    // A multisampled dependency resolves between matching reduced resources.
    const auto msfmt=DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{msfmt,2,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&quality,sizeof quality))&&quality.NumQualityLevels);
    auto msdesc=a->GetDesc();msdesc.Format=msfmt;msdesc.SampleDesc={2,0};D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    test::ComPtr<ID3D12Resource> ms;REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&msdesc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&ms))));
    auto resolved=test::CreateTexture(dev,w,h,msfmt,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);REQUIRE(resolved);
    const auto msid=na::observe::IdOf(ms.Get()),rid=na::observe::IdOf(resolved.Get());p->multisample=msid;
    D3D12_RENDER_TARGET_VIEW_DESC msv{};msv.Format=msfmt;msv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DMS;dev->CreateRenderTargetView(ms.Get(),&msv,av);
    {
        na::observe::HostWorkScope host;
        group=SceneGroup::Create(dev,2,{w,h},{rw,rh},{{msid,ms.Get(),Role::Color,{rw,rh}},{rid,resolved.Get(),Role::Intermediate,{rw,rh}}},error);REQUIRE(group);
        TransferSpec s;s.group=group;s.kind=Kind::ClearColor;s.destination=msid;s.color=msv;p->clear_ms=PreparedTransfer::Create(dev,s,error);REQUIRE(p->clear_ms);
        s.kind=Kind::Resolve;s.source=msid;s.destination=rid;s.resolve_format=msfmt;p->resolve=PreparedTransfer::Create(dev,s,error);REQUIRE(p->resolve);
    }
    const float value[4]={.25f,.5f,.75f,1};l->ClearRenderTargetView(av,value,0,nullptr);l->ResolveSubresource(resolved.Get(),0,ms.Get(),0,msfmt);
    Readback resolved_rb;resolved_rb.Prepare(dev,group->Find(rid)->reduced.Get());
    {na::observe::HostWorkScope host;resolved_rb.Record(l,group->Find(rid)->reduced.Get());}
    REQUIRE(SUCCEEDED(l->Close()));{na::nr::native::Submission own(d.queue.Get(),1,lists);d.queue->ExecuteCommandLists(1,lists);}d.WaitIdle();
    REQUIRE(SUCCEEDED(resolved_rb.data->Map(0,nullptr,&bytes)));wrong=0;
    for(UINT y=0;y<rh;++y)for(UINT x=0;x<rw;++x){const auto* pixel=static_cast<const unsigned char*>(bytes)+resolved_rb.fp.Offset+y*resolved_rb.fp.Footprint.RowPitch+x*4;
        if(!x&&!y)std::printf("resolved sample %u %u %u %u\n",pixel[0],pixel[1],pixel[2],pixel[3]);
        // UNORM clear/resolve conversion can differ by one code value.
        wrong+=pixel[0]<63||pixel[0]>65||pixel[1]<127||pixel[1]>129||pixel[2]<190||pixel[2]>192||pixel[3]!=255;}
    resolved_rb.data->Unmap(0,nullptr);CHECK(wrong==0);CHECK(p->refused==0);
    na::observe::SetTextureOperationHook({});recording.Reset();recording.list->Close();na::nr::native::Maintain();
    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T n=0;messages->GetMessage(i,nullptr,&n);std::vector<unsigned char> storage(n);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(messages->GetMessage(i,m,&n))&&m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("GPU: %s\n",m->pDescription);CHECK(false);}}
    CHECK(dev->GetDeviceRemovedReason()==S_OK);na_observe_stop();return test::finish("scoped_transfers_test");
}
