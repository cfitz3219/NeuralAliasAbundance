// Real GPU composition, no NVIDIA model. All waits/readbacks are test-only.
#include "test_support.hpp"
#include "host_dx12/nr/fg_composition.hpp"
#include <d3d12sdklayers.h>
#include <array>
#include <cmath>
#include <vector>

using Pixel=std::array<float,4>;
using na::nr::CompositionSurface;
using na::nr::UiComposition;
struct Texture {
    test::ComPtr<ID3D12Resource> texture,upload;
    CompositionSurface surface;
};
int Make(test::Device& d,test::List& list,uint32_t bx,uint32_t by,const std::array<Pixel,3>& values,bool single,Texture& t) {
    const uint32_t w=bx+5,h=by+4;
    const auto format=single ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    t.texture=test::CreateTexture(d.device.Get(),w,h,format,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(t.texture);
    auto desc=t.texture->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total=0;
    d.device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&total);
    t.upload=test::CreateBuffer(d.device.Get(),total,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    REQUIRE(t.upload); void* data=nullptr; REQUIRE(SUCCEEDED(t.upload->Map(0,nullptr,&data)));
    for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
        Pixel p{99,99,99,99};
        if(x>=bx && x<bx+3 && y>=by && y<by+2) p=values[x-bx];
        std::memcpy(static_cast<char*>(data)+y*fp.Footprint.RowPitch+x*(single?4:16),p.data(),single?4:16);
    }
    t.upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=t.upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp;
    dst.pResource=t.texture.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    auto b=test::Transition(t.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list.list->ResourceBarrier(1,&b); t.surface={t.texture.Get(),bx,by}; return 0;
}
int Check(test::Device& d,UiComposition mode) {
    test::List list; REQUIRE(list.Create(d.device.Get()));
    std::array<Pixel,3> full{},hud{},enhanced{},alpha{},expected_full{},expected_hud{};
    for(uint32_t x=0;x<3;++x) {
        const float a=x*.5f;
        hud[x]={2,3,4,.375f}; enhanced[x]={6,7,8,.875f}; alpha[x]={a,0,0,a};
        full[x]={.4f*a+(1-a)*2,.6f*a+(1-a)*3,.8f*a+(1-a)*4,.625f};
        expected_full[x]={.4f*a+(1-a)*6,.6f*a+(1-a)*7,.8f*a+(1-a)*8,.625f};
        expected_hud[x]={6,7,8,.375f};
        if(mode==UiComposition::EqualPixels && x>0) {
            expected_full[x]=full[x]; expected_hud[x]=hud[x];
        }
    }
    Texture b,h,e,a;
    REQUIRE(Make(d,list,1,2,full,false,b)==0);
    REQUIRE(Make(d,list,3,1,hud,false,h)==0);
    REQUIRE(Make(d,list,2,3,enhanced,false,e)==0);
    REQUIRE(Make(d,list,4,2,alpha,mode==UiComposition::AlphaRed,a)==0);
    na::nr::FgComposition packet;
    CHECK(!packet.Record(list.list.Get(),100,2,b.surface,h.surface,e.surface,a.surface,mode));
    REQUIRE(packet.Record(list.list.Get(),3,2,b.surface,h.surface,e.surface,a.surface,mode));
    CHECK(!packet.Record(list.list.Get(),3,2,b.surface,h.surface,e.surface,a.surface,mode));
    test::ComPtr<ID3D12Resource> readback[2]; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]{};
    ID3D12Resource* outputs[]={packet.Full(),packet.Hudless()};
    for(uint32_t i=0;i<2;++i) {
        auto desc=outputs[i]->GetDesc(); UINT64 total=0;
        d.device->GetCopyableFootprints(&desc,0,1,0,&fp[i],nullptr,nullptr,&total);
        readback[i]=test::CreateBuffer(d.device.Get(),total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        REQUIRE(readback[i]);
        auto barrier=test::Transition(outputs[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list.list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=outputs[i]; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback[i].Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=fp[i];
        list.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    REQUIRE(SUCCEEDED(list.list->Close()));
    ID3D12CommandList* lists[]={list.list.Get()}; d.queue->ExecuteCommandLists(1,lists); d.WaitIdle();
    REQUIRE(d.device->GetDeviceRemovedReason()==S_OK);
    for(uint32_t i=0;i<2;++i) {
        void* data=nullptr; REQUIRE(SUCCEEDED(readback[i]->Map(0,nullptr,&data)));
        for(uint32_t y=0;y<2;++y) for(uint32_t x=0;x<3;++x) {
            Pixel actual{}; std::memcpy(actual.data(),static_cast<char*>(data)+y*fp[i].Footprint.RowPitch+x*16,16);
            const auto& expected=i?expected_hud[x]:expected_full[x];
            for(uint32_t c=0;c<4;++c) CHECK(std::fabs(actual[c]-expected[c])<.00001f);
        }
        readback[i]->Unmap(0,nullptr);
    }
    std::printf("composition mode %u: transparent/translucent/opaque, independent origins, HDR values and alpha checked\n",static_cast<unsigned>(mode));
    return 0;
}
int main() {
    test::ComPtr<ID3D12Debug> debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    test::Device d; if(!test::CreateDevice(d)) return test::SkipNoDevice("nr_fg_composition_test");
    test::ComPtr<ID3D12InfoQueue> info; d.device.As(&info);
    for(auto mode:{UiComposition::EqualPixels,UiComposition::AlphaRed,UiComposition::UiAlpha}) if(int r=Check(d,mode)) return r;
    if(info) for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<char> bytes(size);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); info->GetMessage(i,m,&size);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { std::printf("D3D12: %s\n",m->pDescription); CHECK(false); }
    }
    return test::finish("nr_fg_composition_test");
}
