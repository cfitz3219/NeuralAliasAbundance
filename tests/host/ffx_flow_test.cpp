#include "test_support.hpp"
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include <vector>
#include <cmath>
int main(){
    if(!na::ffx_flow::Built())return test::kSkip;
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;
    if(!na::ffx_flow::DeviceSupported(d.device.Get())){std::puts("SKIP: AMD search needs 32/64-lane hardware waves");return test::kSkip;}
    constexpr unsigned w=512,h=256;na::ffx_flow::PairFlow flow;
    if(!flow.Init(d.device.Get(),w,h)){std::printf("%s\n",flow.Error());return 1;}
    test::List l;REQUIRE(l.Create(d.device.Get()));
    std::vector<test::ComPtr<ID3D12Resource>> uploads;
    auto image=[&](int dx){
        auto r=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        auto up=test::CreateBuffer(d.device.Get(),w*h*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);unsigned* p=nullptr;up->Map(0,nullptr,reinterpret_cast<void**>(&p));
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            const unsigned a=static_cast<unsigned>(static_cast<int>(x)-dx);unsigned v=(a*1664525u+y*1013904223u);v^=v>>13;v*=2246822519u;v^=v>>16;
            p[y*w+x]=0xff000000u|((v&255)*0x010101u);
        }up->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=r.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource=up.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,w,h,1,w*4};
        l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);auto b=test::Transition(r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);l.list->ResourceBarrier(1,&b);uploads.push_back(up);return r;
    };
    auto previous=image(0),current=image(4);
    auto execute=[&](){l.list->Close();ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();l.Reset();};execute();
    auto measure=[&](ID3D12Resource* a,ID3D12Resource* b,float expected){
        CHECK(flow.Record(l.list.Get(),a,b,0));auto gd=flow.Grid()->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;d.device->GetCopyableFootprints(&gd,0,1,0,&fp,nullptr,nullptr,&bytes);
        auto rb=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto tr=test::Transition(flow.Grid(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&tr);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=flow.Grid();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        tr=test::Transition(flow.Grid(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);l.list->ResourceBarrier(1,&tr);flow.Finish(l.list.Get());execute();
        unsigned char* p=nullptr;rb->Map(0,nullptr,reinterpret_cast<void**>(&p));unsigned good=0,n=0;double error=0;
        for(unsigned y=4;y<h/8-4;++y)for(unsigned x=4;x<w/8-4;++x){auto v=reinterpret_cast<const int16_t*>(p+fp.Offset+y*fp.Footprint.RowPitch)+2*x;
            const float e=std::abs(v[0]-expected)+std::abs(static_cast<float>(v[1]));error+=e;++n;if(e<=1)++good;
        }rb->Unmap(0,nullptr);std::printf("pair expected %.1f px: %u/%u within1px, mean L1 %.3f\n",expected,good,n,error/n);CHECK(good>n*9/10);
    };
    measure(current.Get(),previous.Get(),-4);measure(previous.Get(),current.Get(),4);measure(current.Get(),current.Get(),0);
    // A discarded recording must not advance temporal state or invalidate subsequent barriers.
    CHECK(flow.Record(l.list.Get(),current.Get(),previous.Get(),1));flow.Finish(l.list.Get());l.list->Close();l.Reset();
    measure(current.Get(),previous.Get(),-4);CHECK(d.device->GetDeviceRemovedReason()==S_OK);execute();
    return test::finish("ffx_flow_test");
}
