// Actual DX12 component tests. GPU waits/readback are confined to this executable.
#include "test_support.hpp"
#include "host_dx12/effects/donut.hpp"
#include "host_dx12/gpu/services.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/nr/native_fg.hpp"
#if defined(NA_HAVE_NGX_SDK)
#include "fake_ngx_params.hpp"
#endif
#include <array>
#include <algorithm>
#include <chrono>
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
int backend_benchmark(){
#if !defined(NA_HAVE_DONUT) || !defined(NA_HAVE_NVRHI)
    return test::kSkip;
#else
    // Standalone comparison: GPU waits/readbacks are outside CPU timing.
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;
    constexpr unsigned W=640,H=360;
    std::vector<float> data(W*H*4,.1f);for(unsigned i=0;i<W*H;++i)data[i*4+3]=.37f;
    for(unsigned y=H/3;y<H/3+16;++y)for(unsigned x=W/3;x<W/3+16;++x)for(unsigned c=0;c<3;++c)data[(y*W+x)*4+c]=12;
    test::List upload;REQUIRE(upload.Create(d.device.Get()));auto image=input(d,upload,W,H,DXGI_FORMAT_R32G32B32A32_FLOAT,data,4);
    REQUIRE(SUCCEEDED(upload.list->Close()));ID3D12CommandList* up[]={upload.list.Get()};d.queue->ExecuteCommandLists(1,up);d.WaitIdle();
    na::donut::Inputs in;in.color=image.texture.Get();in.format=DXGI_FORMAT_R32G32B32A32_FLOAT;in.width=W;in.height=H;in.seconds=1.f/60;
    na::donut::Options options;options.bloom=true;options.bloom_sigma=8;options.bloom_amount=.5f;
    na::donut::Pass nativePass,nvrhiPass;
    test::List native;REQUIRE(native.Create(d.device.Get()));REQUIRE(SUCCEEDED(native.list->Close()));
    na::gpu::Recording nvrhi;
    std::shared_ptr<na::donut::Frame> last[2];std::vector<double> samples[2];
    for(unsigned round=0;round<9;++round)for(unsigned step=0;step<2;++step){
        const unsigned backend=(step+round)%2;na::gpu::SetBackendForTests(backend?na::gpu::TestBackend::Nvrhi:na::gpu::TestBackend::Native);
        auto& pass=backend?nvrhiPass:nativePass;
        pass.Prepare(d.device.Get(),d.queue.Get(),in,options);REQUIRE(pass.WaitPrepared(30000));
        double elapsed=0;
        for(unsigned frame=0;frame<40;++frame){
            last[backend].reset();const auto begin=std::chrono::steady_clock::now();
            auto work=pass.Prepare(d.device.Get(),d.queue.Get(),in,options);REQUIRE(work);
            ID3D12GraphicsCommandList* list=nullptr;
            if(backend){REQUIRE(nvrhi.Open(d.device.Get(),d.queue.Get()));list=nvrhi.Native();}
            else {REQUIRE(SUCCEEDED(native.alloc->Reset())&&SUCCEEDED(native.list->Reset(native.alloc.Get(),nullptr)));list=native.list.Get();}
            REQUIRE(work->Record(list));
            if(backend){REQUIRE(nvrhi.Close());nvrhi.Retain(work);REQUIRE(nvrhi.Submit());}
            else {REQUIRE(SUCCEEDED(list->Close()));ID3D12CommandList* lists[]={list};d.queue->ExecuteCommandLists(1,lists);}
            const auto end=std::chrono::steady_clock::now();
            if(frame>=8)elapsed+=std::chrono::duration<double,std::micro>(end-begin).count();
            d.WaitIdle();na::gpu::Collect();last[backend]=std::move(work);
        }
        samples[backend].push_back(elapsed/32);
    }
    for(unsigned backend=0;backend<2;++backend){std::sort(samples[backend].begin(),samples[backend].end());
        std::printf("backend benchmark actual Bloom: %s median CPU prepare+record+submit %.3f us, 9 alternating batches, 32 warm frames/batch\n",backend?"NVRHI":"native",samples[backend][4]);}
    // J2/NR export a list and record their existing native commands into it.
    // Compare that wrapper separately from the effect compute abstraction.
    std::vector<double> recordOnly[2];
    for(unsigned round=0;round<9;++round)for(unsigned step=0;step<2;++step){
        const unsigned backend=(step+round)%2;na::gpu::SetBackendForTests(backend?na::gpu::TestBackend::Nvrhi:na::gpu::TestBackend::Native);
        double elapsed=0;
        for(unsigned frame=0;frame<40;++frame){
            const auto begin=std::chrono::steady_clock::now();ID3D12GraphicsCommandList* list=nullptr;
            if(backend){REQUIRE(nvrhi.Open(d.device.Get(),d.queue.Get()));list=nvrhi.Native();}
            else {REQUIRE(SUCCEEDED(native.alloc->Reset())&&SUCCEEDED(native.list->Reset(native.alloc.Get(),nullptr)));list=native.list.Get();}
            auto barrier=test::Transition(image.texture.Get(),Read,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&barrier);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
            if(backend){REQUIRE(nvrhi.Close());nvrhi.Retain(last[backend]);REQUIRE(nvrhi.Submit());}
            else {REQUIRE(SUCCEEDED(list->Close()));ID3D12CommandList* lists[]={list};d.queue->ExecuteCommandLists(1,lists);}
            const auto end=std::chrono::steady_clock::now();if(frame>=8)elapsed+=std::chrono::duration<double,std::micro>(end-begin).count();
            d.WaitIdle();na::gpu::Collect();
        }
        recordOnly[backend].push_back(elapsed/32);
    }
    for(unsigned backend=0;backend<2;++backend){std::sort(recordOnly[backend].begin(),recordOnly[backend].end());
        std::printf("backend benchmark native-command wrapper: %s median CPU record+submit %.3f us\n",backend?"NVRHI":"native",recordOnly[backend][4]);}
    std::vector<float> result[2];
    for(unsigned backend=0;backend<2;++backend){test::List read;REQUIRE(read.Create(d.device.Get()));auto rb=capture(d,read,last[backend]->Color());
        REQUIRE(SUCCEEDED(read.list->Close()));ID3D12CommandList* lists[]={read.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();result[backend]=pixels(rb);}
    REQUIRE(result[0].size()==result[1].size());double error=0;for(size_t i=0;i<result[0].size();++i)error=(std::max)(error,double(std::abs(result[0][i]-result[1][i])));
    std::printf("backend benchmark output maximum error %.9f; GPU errors=%llu\n",error,static_cast<unsigned long long>(na::gpu::GetStats().errors));
    CHECK(error<.0001&&na::gpu::GetStats().errors==0);CHECK(d.device->GetDeviceRemovedReason()==S_OK);
    na::observe::SetProcessExiting();return test::finish("NVRHI/native backend benchmark");
#endif
}
int main(int argc,char** argv){
    if(argc>1&&std::strcmp(argv[1],"--benchmark")==0)return backend_benchmark();
    const bool nvrhi=argc>1&&std::strcmp(argv[1],"--nvrhi")==0;
    if(nvrhi&&!na::gpu::GetStats().built)return test::kSkip;
    na::gpu::SetBackendForTests(nvrhi?na::gpu::TestBackend::Nvrhi:na::gpu::TestBackend::Native);
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
        test::List l;na::gpu::Recording recording;
        if(nvrhi){if(!recording.Open(d.device.Get(),d.queue.Get())){CHECK(false);return std::vector<float>{};}l.list=recording.Native();}
        else if(!l.Create(d.device.Get())){CHECK(false);return std::vector<float>{};}
        auto frame=pass.Prepare(d.device.Get(),d.queue.Get(),inputs,o);if(!frame){CHECK(false);return std::vector<float>{};}
        CHECK(frame->Record(l.list.Get()));auto rb=capture(d,l,frame->Color());auto original=capture(d,l,image.texture.Get());
        if(nvrhi){CHECK(recording.Close());recording.Retain(frame);CHECK(recording.Submit());}
        else {CHECK(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);}
        d.WaitIdle();na::gpu::Collect();
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
    auto tone=render(options,in);REQUIRE(tone.size()==data.size());CHECK(tone[(20*W+32)*4]<data[(20*W+32)*4]); // highlights compressed
    // The scene average keeps the game's brightness (a dark menu must not be lit up).
    CHECK(std::abs(tone[(3*W+3)*4]-.1f)<.002f);
    for(size_t i=0;i<tone.size();++i)CHECK(std::isfinite(tone[i]));
    options.curve=1;auto filmic=render(options,in);CHECK(filmic!=tone);
    for(size_t i=0;i<filmic.size();i+=4){CHECK(filmic[i]>=0&&filmic[i]<=1);CHECK(filmic[i+3]==.37f);}
    // Adaptation follows a changed scene gradually on the same ordered queue; reset is immediate.
    // While it lags behind a brighter scene, highlights are compressed more; once adapted (or
    // on reset) a uniform scene keeps its own brightness.
    options.curve=0;in.reset=true;render(options,in);
    test::List brightUpload;REQUIRE(brightUpload.Create(d.device.Get()));std::vector<float> brightData(W*H*4,2);
    auto brightImage=input(d,brightUpload,W,H,DXGI_FORMAT_R32G32B32A32_FLOAT,brightData,4);
    REQUIRE(SUCCEEDED(brightUpload.list->Close()));ID3D12CommandList* bl[]={brightUpload.list.Get()};d.queue->ExecuteCommandLists(1,bl);d.WaitIdle();
    auto brighter=in;brighter.color=brightImage.texture.Get();brighter.reset=false;
    auto transition1=render(options,brighter),transition2=render(options,brighter);
    CHECK(transition1[0]<transition2[0]);brighter.reset=true;auto cut=render(options,brighter);CHECK(cut[0]>transition2[0]);
    CHECK(std::abs(cut[0]-2.f)<.02f);
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
    if(nvrhi){const auto gpu=na::gpu::GetStats();CHECK(gpu.submits>0&&gpu.dispatches>0&&gpu.textures>0&&gpu.errors==0);}
    CHECK(d.device->GetDeviceRemovedReason()==S_OK);
    if(info){unsigned errors=0;for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);
        std::vector<uint8_t> storage(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(info->GetMessage(i,m,&bytes))&&m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::printf("D3D12: %s\n",m->pDescription);}}
        CHECK(errors==0);}
    return test::finish("Donut lighting");
#endif
}
