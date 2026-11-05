// Actual DX12 component tests. GPU waits/readback are confined to this executable.
#include "test_support.hpp"
#include "host_dx12/effects/donut.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/nr/native_fg.hpp"
#if defined(NA_HAVE_NGX_SDK)
#include "fake_ngx_params.hpp"
#endif
#include <array>
#include <cmath>
#include <vector>
#include <d3d12sdklayers.h>
namespace {
constexpr auto Read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Image {
    test::ComPtr<ID3D12Resource> texture,upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};
Image input(test::Device& d,test::List& list,unsigned w,unsigned h,DXGI_FORMAT format,
            const std::vector<float>& values,unsigned channels) {
    Image image;image.texture=test::CreateTexture(d.device.Get(),w,h,format,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    if(!image.texture){CHECK(false);return image;}
    UINT64 bytes;auto td=image.texture->GetDesc();d.device->GetCopyableFootprints(&td,0,1,0,&image.footprint,nullptr,nullptr,&bytes);
    image.upload=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped=nullptr;if(!image.upload||FAILED(image.upload->Map(0,nullptr,&mapped))){CHECK(false);return image;}
    for(unsigned y=0;y<h;++y)std::memcpy(static_cast<uint8_t*>(mapped)+y*image.footprint.Footprint.RowPitch,
        values.data()+size_t(y)*w*channels,size_t(w)*channels*sizeof(float));
    image.upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=image.upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=image.footprint;
    dst.pResource=image.texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    auto b=test::Transition(image.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,Read);list.list->ResourceBarrier(1,&b);return image;
}
struct Readback {
    test::ComPtr<ID3D12Resource> buffer;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    unsigned w=0,h=0;
};
Readback capture(test::Device& d,test::List& l,ID3D12Resource* image) {
    Readback out;auto desc=image->GetDesc();out.w=unsigned(desc.Width);out.h=desc.Height;UINT64 bytes;
    d.device->GetCopyableFootprints(&desc,0,1,0,&out.footprint,nullptr,nullptr,&bytes);
    out.buffer=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    if(!out.buffer){CHECK(false);return out;}
    auto b=test::Transition(image,Read,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=image;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=out.buffer.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=out.footprint;
    l.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l.list->ResourceBarrier(1,&b);return out;
}
std::vector<float> pixels(const Readback& rb) {
    std::vector<float> values(size_t(rb.w)*rb.h*4);void* mapped=nullptr;
    if(!rb.buffer||FAILED(rb.buffer->Map(0,nullptr,&mapped))){CHECK(false);return {};}
    for(unsigned y=0;y<rb.h;++y)std::memcpy(values.data()+size_t(y)*rb.w*4,
        static_cast<uint8_t*>(mapped)+y*rb.footprint.Footprint.RowPitch,size_t(rb.w)*16);
    rb.buffer->Unmap(0,nullptr);return values;
}

void sink(const na_event_batch*,void*){}
}
int main(){
#if !defined(NA_HAVE_DONUT)
    return test::kSkip;
#else
    test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;
    test::ComPtr<ID3D12InfoQueue> info;d.device.As(&info);
    na::donut::Pass pass;na::donut::Options options;
    CHECK(!pass.Prepare(d.device.Get(),d.queue.Get(),{},options));CHECK(pass.GetStats().allocations==0);
    constexpr unsigned W=68,H=40;std::vector<float> data(W*H*4);
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){auto i=(y*W+x)*4;data[i]=data[i+1]=data[i+2]=.1f;data[i+3]=.37f;}
    for(unsigned y=17;y<23;++y)for(unsigned x=29;x<35;++x){auto i=(y*W+x)*4;data[i]=data[i+1]=data[i+2]=12.f;}
    test::List upload;REQUIRE(upload.Create(d.device.Get()));auto image=input(d,upload,W,H,DXGI_FORMAT_R32G32B32A32_FLOAT,data,4);
    REQUIRE(SUCCEEDED(upload.list->Close()));ID3D12CommandList* up[]={upload.list.Get()};d.queue->ExecuteCommandLists(1,up);d.WaitIdle();
    na::donut::Inputs in;in.color=image.texture.Get();in.format=DXGI_FORMAT_R32G32B32A32_FLOAT;in.width=W;in.height=H;in.seconds=.1f;
    options.bloom=true;options.bloom_amount=.5f;options.bloom_sigma=8;
    CHECK(!pass.Prepare(d.device.Get(),d.queue.Get(),in,options));REQUIRE(pass.WaitPrepared(30000));
    auto render=[&](const na::donut::Options& o,const na::donut::Inputs& inputs){
        test::List l;if(!l.Create(d.device.Get())){CHECK(false);return std::vector<float>{};}
        auto frame=pass.Prepare(d.device.Get(),d.queue.Get(),inputs,o);if(!frame){CHECK(false);return std::vector<float>{};}
        CHECK(frame->Record(l.list.Get()));auto rb=capture(d,l,frame->Color());auto original=capture(d,l,image.texture.Get());
        CHECK(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        CHECK(pixels(original)==data);return pixels(rb);
    };
    auto bloom=render(options,in);REQUIRE(bloom.size()==data.size());
    CHECK(bloom[(20*W+25)*4]>.1f+.005f); // glow spreads beyond the bright object
    CHECK(std::abs(bloom[(3*W+3)*4]-.1f)<.002f); // distant dark scene stays dark
    for(size_t i=3;i<bloom.size();i+=4)CHECK(bloom[i]==.37f);
    const auto warm=pass.GetStats();auto repeated=render(options,in);CHECK(repeated==bloom);
    CHECK(pass.GetStats().allocations==warm.allocations);CHECK(pass.GetStats().bindings==warm.bindings);
    options.bloom_amount=0;CHECK(!pass.Prepare(d.device.Get(),d.queue.Get(),in,options));
    options.bloom_amount=.5f;options.bloom_threshold=100;auto noGlow=render(options,in);CHECK(noGlow==data);
    options.bloom=false;options.tone=true;in.reset=true;
    auto tone=render(options,in);REQUIRE(tone.size()==data.size());CHECK(tone[(20*W+32)*4]!=data[(20*W+32)*4]);
    for(size_t i=0;i<tone.size();++i)CHECK(std::isfinite(tone[i]));
    options.curve=1;auto filmic=render(options,in);CHECK(filmic!=tone);
    for(size_t i=0;i<filmic.size();i+=4){CHECK(filmic[i]>=0&&filmic[i]<=1);CHECK(filmic[i+3]==.37f);}
    // Exposure follows a changed scene gradually on the same ordered queue; reset is immediate.
    options.curve=0;in.reset=true;render(options,in);
    test::List brightUpload;REQUIRE(brightUpload.Create(d.device.Get()));std::vector<float> brightData(W*H*4,2);
    auto brightImage=input(d,brightUpload,W,H,DXGI_FORMAT_R32G32B32A32_FLOAT,brightData,4);
    REQUIRE(SUCCEEDED(brightUpload.list->Close()));ID3D12CommandList* bl[]={brightUpload.list.Get()};d.queue->ExecuteCommandLists(1,bl);d.WaitIdle();
    auto brighter=in;brighter.color=brightImage.texture.Get();brighter.reset=false;
    auto transition1=render(options,brighter),transition2=render(options,brighter);
    CHECK(transition1[0]>transition2[0]);brighter.reset=true;auto cut=render(options,brighter);CHECK(cut[0]<transition2[0]);
    options.amount=0;options.bloom=true;options.bloom_amount=0;
    CHECK(!pass.Prepare(d.device.Get(),d.queue.Get(),in,options));
    options.bloom_amount=.1f;options.bloom_threshold=100;
    in.width=51;in.height=27;in.x=3;in.y=5;in.preserve_extent=true;
    auto subrect=render(options,in);CHECK(subrect==data); // no bloom, all unused native texels preserved
    in.x=W;CHECK(!pass.Prepare(d.device.Get(),d.queue.Get(),in,options));
    in.x=3;options.bloom_sigma=std::nanf("");CHECK(!pass.Prepare(d.device.Get(),d.queue.Get(),in,options));
    // sRGB preserves the original encoding, alpha and colour when no highlights contribute.
    {test::List l;REQUIRE(l.Create(d.device.Get()));constexpr unsigned SW=17,SH=13;const uint32_t rgba=0x80604020;
        std::vector<float> packed(SW*SH);for(auto& v:packed)std::memcpy(&v,&rgba,4);
        auto srgbImage=input(d,l,SW,SH,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,packed,1);
        na::donut::Inputs si;si.color=srgbImage.texture.Get();si.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;si.width=SW;si.height=SH;si.srgb=true;
        options.bloom_sigma=8;options.bloom_threshold=100;auto f=pass.Prepare(d.device.Get(),d.queue.Get(),si,options);REQUIRE(f);
        CHECK(f->Color()->GetDesc().Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);REQUIRE(f->Record(l.list.Get()));auto rb=capture(d,l,f->Color());
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();void* map=nullptr;
        REQUIRE(SUCCEEDED(rb.buffer->Map(0,nullptr,&map)));for(unsigned y=0;y<SH;++y)for(unsigned x=0;x<SW;++x){
            uint32_t value;std::memcpy(&value,static_cast<uint8_t*>(map)+y*rb.footprint.Footprint.RowPitch+x*4,4);CHECK(value==rgba);}
        rb.buffer->Unmap(0,nullptr);}
#if defined(NA_HAVE_NGX_SDK)
    // Native adapter replaces only Color, restores it on scope exit, and retains GPU payloads.
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),false));
    na_observe_config config{};config.size_bytes=sizeof(config);REQUIRE(na_observe_start(&config,sink,nullptr)==NA_OK);
    test::List native;REQUIRE(native.Create(d.device.Get()));test::FakeParams parameters;
    parameters.Set("Color",image.texture.Get());parameters.Set("Width",W);parameters.Set("Height",H);
    parameters.Set("DLSS.Render.Subrect.Dimensions.Width",W-10);parameters.Set("DLSS.Render.Subrect.Dimensions.Height",H-8);
    parameters.Set("DLSS.Input.Color.Subrect.Base.X",3u);parameters.Set("DLSS.Input.Color.Subrect.Base.Y",4u);
    auto& settings=na::settings::Get();settings.donut_tone=1;
    {na::donut::NativeScope warmScope(native.list.Get(),&parameters);}REQUIRE(na::donut::ScenePass().WaitPrepared(30000));
    ID3D12Resource* changed=nullptr;
    {na::donut::NativeScope scope(native.list.Get(),&parameters);REQUIRE(NVSDK_NGX_SUCCEED(parameters.Get("Color",&changed)));
        REQUIRE(changed&&changed!=image.texture.Get());CHECK(changed->GetDesc().Width==W&&changed->GetDesc().Height==H);
        CHECK(parameters.U("DLSS.Input.Color.Subrect.Base.X")==3);}
    ID3D12Resource* restored=nullptr;parameters.Get("Color",&restored);CHECK(restored==image.texture.Get());
    auto nativePixels=capture(d,native,changed);REQUIRE(SUCCEEDED(native.list->Close()));ID3D12CommandList* nativeLists[]={native.list.Get()};
    {na::nr::native::Submission submission(d.queue.Get(),1,nativeLists);d.queue->ExecuteCommandLists(1,nativeLists);}d.WaitIdle();
    auto nv=pixels(nativePixels);CHECK(nv[0]==data[0]);CHECK(nv[(20*W+32)*4]!=data[(20*W+32)*4]);
    settings.donut_tone=0;REQUIRE(na_observe_stop()==NA_OK);na::observe::SetProcessExiting();
#endif
    CHECK(d.device->GetDeviceRemovedReason()==S_OK);
    if(info){unsigned errors=0;for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);
        std::vector<uint8_t> storage(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(info->GetMessage(i,m,&bytes))&&m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::printf("D3D12: %s\n",m->pDescription);}}
        CHECK(errors==0);}
    return test::finish("Donut lighting");
#endif
}
