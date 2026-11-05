// Offline actual-DX12 XeGTAO pixels. All waits/readbacks are test-only.
#include "test_support.hpp"
#include "host_dx12/effects/xegtao.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/fg/camera.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "providers/camera/matrix_candidates.hpp"
#if defined(NA_HAVE_NGX_SDK)
#include "fake_ngx_params.hpp"
#endif
#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include <d3d12sdklayers.h>

namespace {
constexpr auto Read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
na::xegtao::Projection projection(bool reverse) {
    const double a=reverse?0.:1., b=reverse?.1:-.1;
    const double m[16]={1,0,0,0,0,1.5,0,0,0,0,a,b,0,0,1,0};
    na::xegtao::Projection p;CHECK(na::xegtao::FromViewProjection(m,p));return p;
}
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
#if defined(NA_HAVE_NGX_SDK)
int native(test::Device& d) {
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),false));
    na_observe_config config{};config.size_bytes=sizeof(config);REQUIRE(na_observe_start(&config,sink,nullptr)==NA_OK);
    test::List list;REQUIRE(list.Create(d.device.Get()));
    constexpr unsigned W=81,H=49; // odd render subrect, separate native color/depth bases
    std::vector<float> colors(size_t(W)*H*4,4),depths(size_t(W)*H,.95f);
    auto color=input(d,list,W,H,DXGI_FORMAT_R32G32B32A32_FLOAT,colors,4);
    auto depth=input(d,list,W,H,DXGI_FORMAT_R32_FLOAT,depths,1);REQUIRE(color.texture&&depth.texture);
    na::fg::Camera camera{};camera.view_to_clip[0]=1;camera.view_to_clip[5]=float(W)/H;
    camera.view_to_clip[10]=1;camera.view_to_clip[11]=1;camera.view_to_clip[14]=-.1f;
    na::camera::Matrix row{},inverse{};for(unsigned i=0;i<16;++i)row[i]=camera.view_to_clip[i];
    REQUIRE(na::camera::Invert(row,inverse));for(unsigned i=0;i<16;++i)camera.clip_to_view[i]=float(inverse[i]);
    for(unsigned i:{0u,5u,10u,15u})camera.clip_to_previous[i]=camera.previous_to_clip[i]=1;
    camera.near_plane=.1f;camera.far_plane=10000;camera.fov=1;camera.aspect=float(W)/H;
    camera.camera_motion=1;camera.right[0]=camera.up[1]=camera.forward[2]=1;
    REQUIRE(na::fg::ValidCamera(camera));na::fg::RecordCamera(43,7,camera);
    na::fg::EvaluationScope evaluation(42,7);
    test::FakeParams parameters;
    parameters.Set("Color",color.texture.Get());parameters.Set("Depth",depth.texture.Get());
    parameters.Set("Width",W);parameters.Set("Height",H);
    parameters.Set("DLSS.Render.Subrect.Dimensions.Width",W-12);parameters.Set("DLSS.Render.Subrect.Dimensions.Height",H-8);
    parameters.Set("DLSS.Input.Color.Subrect.Base.X",4u);parameters.Set("DLSS.Input.Color.Subrect.Base.Y",3u);
    parameters.Set("DLSS.Input.Depth.Subrect.Base.X",2u);parameters.Set("DLSS.Input.Depth.Subrect.Base.Y",1u);
    parameters.Set("Jitter.Offset.X",0.f);parameters.Set("Jitter.Offset.Y",0.f);
    auto& settings=na::settings::Get();settings.xegtao_enabled=1;
    {na::xegtao::NativeScope prepare(list.list.Get(),&parameters);}
    REQUIRE(na::xegtao::ScenePass().WaitPrepared(30000));
    ID3D12Resource* shaded=nullptr;
    {
        na::xegtao::NativeScope scope(list.list.Get(),&parameters);
        REQUIRE(NVSDK_NGX_SUCCEED(parameters.Get("Color",&shaded)));
        REQUIRE(shaded&&shaded!=color.texture.Get());
        CHECK(shaded->GetDesc().Width==W&&shaded->GetDesc().Height==H);
        CHECK(parameters.U("DLSS.Input.Color.Subrect.Base.X")==4&&parameters.U("DLSS.Input.Color.Subrect.Base.Y")==3);
    }
    ID3D12Resource* restored=nullptr;parameters.Get("Color",&restored);CHECK(restored==color.texture.Get());
    // Copy after restoration: source output still lives through the recording lease.
    auto output=capture(d,list,shaded);auto original=capture(d,list,color.texture.Get());
    REQUIRE(SUCCEEDED(list.list->Close()));ID3D12CommandList* lists[]={list.list.Get()};
    {na::nr::native::Submission submission(d.queue.Get(),1,lists);d.queue->ExecuteCommandLists(1,lists);}d.WaitIdle();
    auto values=pixels(output),unchanged=pixels(original);REQUIRE(values.size()==W*H*4);
    for(float v:unchanged)CHECK(v==4); // native game input was never overwritten
    CHECK(std::isfinite(values[(20*W+30)*4])&&values[(20*W+30)*4]>0);
    settings.xegtao_enabled=0;
    const auto recorded=na::xegtao::ScenePass().GetStats().recorded;
    {na::xegtao::NativeScope off(list.list.Get(),&parameters);}
    CHECK(na::xegtao::ScenePass().GetStats().recorded==recorded);
    REQUIRE(na_observe_stop()==NA_OK);na::observe::SetProcessExiting();return 0;
}
#endif
}
int main() {
    CHECK(na::settings::Settings{}.xegtao_enabled.load()==0);
#if !defined(NA_HAVE_XEGTAO)
    std::printf("XeGTAO: SKIP (DXC shaders not built)\n");return 77;
#endif
    test::Device d;if(!test::CreateDevice(d))return test::SkipNoDevice("XeGTAO");
    test::ComPtr<ID3D12InfoQueue> info;d.device->QueryInterface(IID_PPV_ARGS(&info));
    na::xegtao::Pass pass;na::xegtao::Options options;
    CHECK(!pass.Prepare(d.device.Get(),d.queue.Get(),{},options));
    CHECK(pass.GetStats().allocations==0&&!pass.GetStats().compiling);
    options.enabled=true;options.amount=100;options.radius=.5f;
    constexpr unsigned W=96,H=64,PW=W+16,PH=H+8;
    std::vector<float> colors(size_t(PW)*PH*4,8.f),depths(size_t(PW)*PH,1.f);
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
        auto i=(size_t(y+4)*PW+x+7)*4;colors[i]=4;colors[i+1]=.25f;colors[i+2]=.125f;colors[i+3]=.37f;
    }
    std::vector<float> reference;
    std::vector<std::shared_ptr<na::xegtao::Frame>> retained;
    for(unsigned example=0;example<3;++example){
        const bool reverse=example==1,box=example<2;
        // A near box against a flat plane: expect contact occlusion adjacent to it,
        // neutral distant plane, and unmodified clear-depth sky at the top edge.
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
            const float z=box&&x>30&&x<53&&y>20&&y<45?1.8f:2.f;
            depths[size_t(y+2)*PW+x+3]=y<3?(reverse?0.f:1.f):(reverse?.1f/z:1-.1f/z);
        }
        test::List l;REQUIRE(l.Create(d.device.Get()));
        auto color=input(d,l,PW,PH,DXGI_FORMAT_R32G32B32A32_FLOAT,colors,4);
        auto depth=input(d,l,PW,PH,DXGI_FORMAT_R32_FLOAT,depths,1);REQUIRE(color.texture&&depth.texture);
        na::xegtao::Inputs in;in.color=color.texture.Get();in.depth=depth.texture.Get();in.width=W;in.height=H;
        in.color_x=7;in.color_y=4;in.depth_x=3;in.depth_y=2;in.clear_depth=reverse?0.f:1.f;
        in.color_format=DXGI_FORMAT_R32G32B32A32_FLOAT;in.projection=projection(reverse);
        auto frame=pass.Prepare(d.device.Get(),d.queue.Get(),in,options);
        if(!frame){
            const bool ready=pass.WaitPrepared(30000);
            if(!ready)std::printf("XeGTAO prepare: %s\n",pass.GetStats().reason);
            REQUIRE(ready);frame=pass.Prepare(d.device.Get(),d.queue.Get(),in,options);
        }
        REQUIRE(frame);retained.push_back(frame);REQUIRE(frame->Record(l.list.Get()));
        auto rb=capture(d,l,frame->Color());
        auto view_z=capture(d,l,frame->ViewDepthForTests());
        auto normal=capture(d,l,frame->NormalsForTests());
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        auto output=pixels(rb);REQUIRE(output.size()==W*H*4);
        void* diagnostic=nullptr;
        REQUIRE(SUCCEEDED(view_z.buffer->Map(0,nullptr,&diagnostic)));
        auto* zrow=reinterpret_cast<const float*>(static_cast<const uint8_t*>(diagnostic)+32*view_z.footprint.Footprint.RowPitch);
        CHECK(std::abs(zrow[40]-(box?1.8f:2.f))<1e-4f&&std::abs(zrow[85]-2.f)<1e-4f);
        std::printf("view Z box %.5f plane %.5f\n",zrow[40],zrow[85]);view_z.buffer->Unmap(0,nullptr);
        REQUIRE(SUCCEEDED(normal.buffer->Map(0,nullptr,&diagnostic)));
        auto* nrow=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(diagnostic)+32*normal.footprint.Footprint.RowPitch);
        std::printf("normal box %08x plane %08x\n",nrow[40],nrow[85]);normal.buffer->Unmap(0,nullptr);
        float darkest=4,flat=0;unsigned flat_count=0;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
            const auto i=(size_t(y)*W+x)*4;
            CHECK(std::isfinite(output[i])&&output[i]>=0&&output[i]<=4.001f);
            CHECK(std::abs(output[i+3]-.37f)<1e-6f); // alpha never becomes an AO mask
            if(y<3)CHECK(std::abs(output[i]-4)<1e-5f); // clear sky / HDR preserved
            else darkest=std::min(darkest,output[i]);
            if(x>75&&x<90&&y>10&&y<53){flat+=output[i];++flat_count;}
        }
        flat/=flat_count;
        std::printf("XeGTAO %s %s: darkest %.6f distant plane %.6f (HDR input4)\n",reverse?"reverse":"forward",box?"box":"flat",darkest,flat);
        std::printf("RGB red center %.4f side %.4f far %.4f\n",output[(32*W+40)*4],output[(32*W+55)*4],output[(32*W+85)*4]);
        if(box)CHECK(darkest<3.7f);
        if(!box){
            CHECK(std::abs(output[(32*W+40)*4]-4)<.02f);
            // A shallow foreground ledge must add local contact shading; a
            // distant region stays at Intel's same baseline approximation.
            CHECK(reference[(32*W+55)*4]<output[(32*W+55)*4]-.5f);
            CHECK(std::abs(reference[(32*W+85)*4]-output[(32*W+85)*4])<.02f);
        }
        if(reference.empty())reference=output;
        else if(reverse){float difference=0;for(size_t i=0;i<output.size();++i)difference=std::max(difference,std::abs(output[i]-reference[i]));
            std::printf("forward/reverse max difference %.6f\n",difference);CHECK(difference<.04f);}
    }
    // A warm, retired packet reuses every immutable descriptor binding.
    {na::xegtao::Pass cached;test::List l;REQUIRE(l.Create(d.device.Get()));
        std::vector<float> c(64*32*4,1),z(64*32,1);auto color=input(d,l,64,32,DXGI_FORMAT_R32G32B32A32_FLOAT,c,4);
        auto depth=input(d,l,64,32,DXGI_FORMAT_R32_FLOAT,z,1);na::xegtao::Inputs ci;ci.color=color.texture.Get();ci.depth=depth.texture.Get();
        ci.width=64;ci.height=32;ci.color_format=DXGI_FORMAT_R32G32B32A32_FLOAT;ci.projection=projection(false);ci.clear_depth=1;
        cached.Prepare(d.device.Get(),d.queue.Get(),ci,options);REQUIRE(cached.WaitPrepared(30000));
        auto first=cached.Prepare(d.device.Get(),d.queue.Get(),ci,options);REQUIRE(first&&first->Record(l.list.Get()));
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        first.reset();const auto before=cached.GetStats();auto next=cached.Prepare(d.device.Get(),d.queue.Get(),ci,options);REQUIRE(next);
        CHECK(cached.GetStats().bindings==before.bindings);CHECK(cached.GetStats().allocations==before.allocations);
        // Changing the denoiser output binding still invalidates the cache.
        next.reset();options.denoise=2;
        REQUIRE(cached.Prepare(d.device.Get(),d.queue.Get(),ci,options));CHECK(cached.GetStats().bindings==before.bindings+1);options.denoise=1;
    }
    CHECK(pass.GetStats().recorded==3);
    options.amount=0;CHECK(!pass.Prepare(d.device.Get(),d.queue.Get(),{},options));
    CHECK(pass.GetStats().recorded==3);CHECK(d.device->GetDeviceRemovedReason()==S_OK);
    // sRGB encode/decode, alpha, resize, and all Intel denoising choices.
    // Clear sky must round-trip exactly regardless of denoising strength.
    options.amount=100;
    for(unsigned denoise=0;denoise<4;++denoise){
        constexpr unsigned SW=65,SH=33;test::List l;REQUIRE(l.Create(d.device.Get()));
        std::vector<float> packed(SW*SH),sky(SW*SH,1.f);
        const uint32_t rgba=0x80604020;for(auto& pixel:packed)std::memcpy(&pixel,&rgba,4);
        auto color=input(d,l,SW,SH,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,packed,1);
        auto depth=input(d,l,SW,SH,DXGI_FORMAT_R32_FLOAT,sky,1);
        na::xegtao::Inputs in;in.color=color.texture.Get();in.depth=depth.texture.Get();
        in.width=SW;in.height=SH;in.color_format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;in.srgb=true;
        in.clear_depth=1;in.projection=projection(false);options.denoise=denoise;
        auto frame=pass.Prepare(d.device.Get(),d.queue.Get(),in,options);REQUIRE(frame);
        REQUIRE(frame->Color()->GetDesc().Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        REQUIRE(frame->Record(l.list.Get()));auto rb=capture(d,l,frame->Color());
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        void* map=nullptr;REQUIRE(SUCCEEDED(rb.buffer->Map(0,nullptr,&map)));
        const uint8_t expected[]={32,64,96,128};
        for(unsigned y=0;y<SH;++y)for(unsigned x=0;x<SW;++x)for(unsigned c=0;c<4;++c){
            const auto value=static_cast<const uint8_t*>(map)[y*rb.footprint.Footprint.RowPitch+x*4+c];
            CHECK(std::abs(int(value)-expected[c])<2);
        }
        rb.buffer->Unmap(0,nullptr);
    }
#if defined(NA_HAVE_NGX_SDK)
    REQUIRE(native(d)==0);
#endif
    if(info){unsigned errors=0;for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
        SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if(SUCCEEDED(info->GetMessage(i,message,&size))&&(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)){
            ++errors;std::printf("D3D12: %s\n",message->pDescription);
        }
    }CHECK(errors==0);}
    return test::finish("XeGTAO");
}
