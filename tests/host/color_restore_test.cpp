// Actual full-output spatial bypass when a reduced input cannot use DLSS.
// Preserves HDR values, samples non-integral extents, then draws HUD at native
// pixels. This is intentionally a linear scaler, not a DLSS quality fixture.
#include "test_support.hpp"
#include "host_dx12/scale/depth_restore.hpp"
#include <cmath>
int main(int argc,char** argv)
{
    test::Device d;
    if(argc>1&&!std::strcmp(argv[1],"--warp")) {
        REQUIRE(SUCCEEDED(CreateDXGIFactory2(0,IID_PPV_ARGS(&d.factory))));
        test::ComPtr<IDXGIAdapter> warp;REQUIRE(SUCCEEDED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
        REQUIRE(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d.device))));
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q,IID_PPV_ARGS(&d.queue))));
        REQUIRE(SUCCEEDED(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&d.fence))));
        d.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);REQUIRE(d.event);d.warp=true;
        std::printf("device: WARP (forced)\n");
    } else if(!test::CreateDevice(d))return test::SkipNoDevice("color_restore_test");
    auto* dev=d.device.Get();
    constexpr UINT iw=8,ih=4,ow=31,oh=17;
    const auto fmt=DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto input=test::CreateTexture(dev,iw,ih,fmt,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    auto output=test::CreateTexture(dev,ow,oh,fmt,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);REQUIRE(input&&output);
    std::string error;auto pass=na::scale::ColorRestore::Create(dev,input.Get(),output.Get(),0,fmt,error);REQUIRE(pass);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT in_fp{},out_fp{};UINT64 in_bytes=0,out_bytes=0;
    const auto id=input->GetDesc(),od=output->GetDesc();
    dev->GetCopyableFootprints(&id,0,1,0,&in_fp,nullptr,nullptr,&in_bytes);dev->GetCopyableFootprints(&od,0,1,0,&out_fp,nullptr,nullptr,&out_bytes);
    auto upload=test::CreateBuffer(dev,in_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto rb=test::CreateBuffer(dev,out_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(upload&&rb);
    void* mapped=nullptr;REQUIRE(SUCCEEDED(upload->Map(0,nullptr,&mapped)));
    for(UINT y=0;y<ih;++y)for(UINT x=0;x<iw;++x){const float p[4]={float(x)/(iw-1),float(y)/(ih-1),4,1};
        std::memcpy(static_cast<uint8_t*>(mapped)+in_fp.Offset+y*in_fp.Footprint.RowPitch+x*16,p,16);}
    upload->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};test::ComPtr<ID3D12DescriptorHeap> heap;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));const auto rtv=heap->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(output.Get(),nullptr,rtv);
    test::List recording;REQUIRE(recording.Create(dev));auto* list=recording.list.Get();
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=in_fp;
    dst.pResource=input.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    auto b=test::Transition(input.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
    REQUIRE(pass->Record(list));
    const float hud[4]={1,0,0,1};const D3D12_RECT box{0,0,5,3};list->ClearRenderTargetView(rtv,hud,1,&box);
    b=test::Transition(output.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
    src={};dst={};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=out_fp;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    REQUIRE(SUCCEEDED(list->Close()));ID3D12CommandList* lists[]{list};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
    const void* data=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,const_cast<void**>(&data))));
    // D3D texture addressing guarantees at least 8 fractional subtexel bits.
    // A one-subdivision coordinate error changes this gradient by <=1/(256*(size-1)).
    // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#TextureAddressingAndLODPrecision
    constexpr float tolerance_x=1.f/(256.f*(iw-1))+1e-6f,tolerance_y=1.f/(256.f*(ih-1))+1e-6f;
    float worst_x=0,worst_y=0;
    for(UINT y=0;y<oh;++y)for(UINT x=0;x<ow;++x){float p[4];std::memcpy(p,static_cast<const uint8_t*>(data)+out_fp.Offset+y*out_fp.Footprint.RowPitch+x*16,16);
        if(x<5&&y<3){CHECK(p[0]==1&&p[1]==0&&p[2]==0&&p[3]==1);continue;}
        const float ex=std::fmin(float(iw-1),std::fmax(0.f,(x+.5f)*iw/ow-.5f))/(iw-1);
        const float ey=std::fmin(float(ih-1),std::fmax(0.f,(y+.5f)*ih/oh-.5f))/(ih-1);
        worst_x=std::fmax(worst_x,std::fabs(p[0]-ex));worst_y=std::fmax(worst_y,std::fabs(p[1]-ey));
        if(!(std::fabs(p[0]-ex)<=tolerance_x&&std::fabs(p[1]-ey)<=tolerance_y&&p[2]==4&&p[3]==1))
            std::printf("pixel %u,%u actual %.9g %.9g %.9g %.9g expected %.9g %.9g 4 1\n",x,y,p[0],p[1],p[2],p[3],ex,ey);
        CHECK(std::fabs(p[0]-ex)<=tolerance_x&&std::fabs(p[1]-ey)<=tolerance_y&&p[2]==4&&p[3]==1);}
    std::printf("gradient max error %.9g/%.9g, addressing bounds %.9g/%.9g\n",worst_x,worst_y,tolerance_x,tolerance_y);
    rb->Unmap(0,nullptr);return test::finish("color_restore_test");
}
