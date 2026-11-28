// The scene sizes come from native DX11 gameplay traces. Synthetic colors make the returned
// current frame, channel order, destination extent and one-pixel HUD independently observable.
// Waiting and mapping below are restricted to this offline test.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/native_d3d11/shared_resources.hpp"
#include <d3dcompiler.h>
#include <cstdio>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace na::native_d3d11;

namespace {
constexpr UINT outW = 3440, outH = 1440;
constexpr char shader[] =
    "float4 vs(uint i:SV_VertexID):SV_Position{float2 p=float2((i<<1)&2,i&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}"
    "Texture2D<float4> image:register(t0);float4 ps(float4 p:SV_Position):SV_Target{return image.Load(int3(p.xy,0));}";

int Exercise(test::Device& d, ID3D11Device* native, ID3D11DeviceContext* ctx,
             UINT w, UINT h, na::dlss::Quality level) {
    SharedFences fences;
    REQUIRE(SUCCEEDED(fences.Create(native,d.device.Get(),d.queue.Get())));
    SharedTexture color, depth, motion, output;
    REQUIRE(SUCCEEDED(color.Create(native,d.device.Get(),{w,h,DXGI_FORMAT_R8G8B8A8_UNORM})));
    REQUIRE(SUCCEEDED(depth.Create(native,d.device.Get(),{w,h,DXGI_FORMAT_R32_FLOAT})));
    REQUIRE(SUCCEEDED(motion.Create(native,d.device.Get(),{w,h,DXGI_FORMAT_R16G16_FLOAT})));
    REQUIRE(SUCCEEDED(output.Create(native,d.device.Get(),{outW,outH,DXGI_FORMAT_R8G8B8A8_UNORM,true})));
    ComPtr<ID3D11ShaderResourceView> resultView;
    REQUIRE(SUCCEEDED(native->CreateShaderResourceView(output.Native(),nullptr,&resultView)));

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=outW; desc.Height=outH; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> screen, before, after;
    ComPtr<ID3D11RenderTargetView> screenView;
    REQUIRE(SUCCEEDED(native->CreateTexture2D(&desc,nullptr,&screen)));
    REQUIRE(SUCCEEDED(native->CreateRenderTargetView(screen.Get(),nullptr,&screenView)));
    desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.BindFlags=0;
    REQUIRE(SUCCEEDED(native->CreateTexture2D(&desc,nullptr,&before)));
    REQUIRE(SUCCEEDED(native->CreateTexture2D(&desc,nullptr,&after)));
    ComPtr<ID3DBlob> vsCode,psCode;
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vsCode,nullptr)));
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&psCode,nullptr)));
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    REQUIRE(SUCCEEDED(native->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)));
    REQUIRE(SUCCEEDED(native->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)));
    const D3D11_VIEWPORT vp{0,0,float(outW),float(outH),0,1};
    std::vector<uint32_t> image(size_t(w)*h), zero(image.size(),0);
    std::vector<float> z(image.size(),0.5f);
    ctx->UpdateSubresource(depth.Native(),0,nullptr,z.data(),w*4,0);
    ctx->UpdateSubresource(motion.Native(),0,nullptr,zero.data(),w*4,0);

    const UINT stripeX=outW-83;
    const uint32_t hud=0xff12c8fa; // literal BGRA bytes
    const std::vector<uint32_t> stripe(outH,hud);
    for (UINT frame=0; frame<2; ++frame) {
        const uint32_t red=0xff1010e0, blue=0xffe01010; // RGBA8 on the shared input
        for (UINT y=0; y<h; ++y) for (UINT x=0; x<w; ++x)
            image[size_t(y)*w+x]=((x>=w/2) != (frame!=0)) ? red : blue;
        ctx->UpdateSubresource(color.Native(),0,nullptr,image.data(),w*4,0);
        uint64_t ready=0,done=0;
        REQUIRE(SUCCEEDED(fences.SignalNative(&ready)) && SUCCEEDED(fences.WaitEffects(ready)));
        test::List list;
        REQUIRE(list.Create(d.device.Get()));
        const auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER barriers[]={
            test::Transition(color.Effects(),D3D12_RESOURCE_STATE_COMMON,read),
            test::Transition(depth.Effects(),D3D12_RESOURCE_STATE_COMMON,read),
            test::Transition(motion.Effects(),D3D12_RESOURCE_STATE_COMMON,read),
            test::Transition(output.Effects(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list.list->ResourceBarrier(4,barriers);
        na::dlss::EvalDesc e{};
        e.color=color.Effects(); e.depth=depth.Effects(); e.motion=motion.Effects(); e.output=output.Effects();
        e.render_w=w; e.render_h=h; e.out_w=outW; e.out_h=outH; e.quality=level;
        e.reset=true; e.auto_exposure=true; e.preset=13;
        REQUIRE(na::dlss::Evaluate(list.list.Get(),e));
        for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        list.list->ResourceBarrier(4,barriers);
        REQUIRE(SUCCEEDED(list.list->Close()));
        ID3D12CommandList* submitted[]={list.list.Get()};
        d.queue->ExecuteCommandLists(1,submitted);
        na::dlss::OnExecuted(d.queue.Get(),1,submitted);
        REQUIRE(SUCCEEDED(fences.SignalEffects(&done)) && SUCCEEDED(fences.WaitNative(done)));

        // Full-size native compositor, followed by the game's independent HUD draw equivalent.
        ctx->OMSetRenderTargets(1,screenView.GetAddressOf(),nullptr);
        ctx->RSSetViewports(1,&vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(),nullptr,0); ctx->PSSetShader(ps.Get(),nullptr,0);
        ctx->PSSetShaderResources(0,1,resultView.GetAddressOf()); ctx->Draw(3,0);
        ID3D11ShaderResourceView* unbound=nullptr;
        ctx->PSSetShaderResources(0,1,&unbound); ctx->OMSetRenderTargets(0,nullptr,nullptr);
        ctx->CopyResource(before.Get(),screen.Get());
        D3D11_BOX box{stripeX,0,0,stripeX+1,outH,1};
        ctx->UpdateSubresource(screen.Get(),0,&box,stripe.data(),4,0);
        ctx->CopyResource(after.Get(),screen.Get());
        D3D11_MAPPED_SUBRESOURCE pre{},post{};
        REQUIRE(SUCCEEDED(ctx->Map(before.Get(),0,D3D11_MAP_READ,0,&pre)));
        REQUIRE(SUCCEEDED(ctx->Map(after.Get(),0,D3D11_MAP_READ,0,&post)));
        size_t hudPixels=0, changedOutside=0, quadrantSize[4]{}, matching[4]{};
        for (UINT y=0; y<outH; ++y) {
            const auto* a=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pre.pData)+size_t(y)*pre.RowPitch);
            const auto* b=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(post.pData)+size_t(y)*post.RowPitch);
            for (UINT x=0; x<outW; ++x) {
                if (x==stripeX) hudPixels+=(b[x]==hud && a[x]!=hud);
                else changedOutside+=(a[x]!=b[x]);
                const UINT q=(x>=outW/2)+2*(y>=outH/2);
                const int r=(a[x]>>16)&255, blueByte=a[x]&255;
                const bool wantRed=(x>=outW/2)!=(frame!=0);
                ++quadrantSize[q];
                matching[q]+=wantRed ? r>blueByte+60 : blueByte>r+60;
            }
        }
        ctx->Unmap(after.Get(),0); ctx->Unmap(before.Get(),0);
        // NGX can draw its enabled diagnostic text over part of the lower left quadrant.
        // Check every pixel's dominant color and require coverage in every quadrant.
        double lowest=100.0;
        for (UINT q=0; q!=4; ++q) lowest=std::min(lowest,100.0*matching[q]/quadrantSize[q]);
        std::printf("%ux%u -> %ux%u frame %u: lowest quadrant match %.3f%%, HUD %zu/%u, changed outside HUD %zu\n",
                    w,h,outW,outH,frame,lowest,hudPixels,outH,changedOutside);
        REQUIRE(lowest>98.0 && hudPixels==outH && changedOutside==0);
        d.WaitIdle();
    }
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    test::Device d;
    if (!test::CreateDevice(d,false)) return test::kSkip;
    ComPtr<IDXGIAdapter1> adapter;
    REQUIRE(SUCCEEDED(d.factory->EnumAdapterByLuid(d.device->GetAdapterLuid(),IID_PPV_ARGS(&adapter))));
    DXGI_ADAPTER_DESC1 desc{};
    REQUIRE(SUCCEEDED(adapter->GetDesc1(&desc)));
    if (desc.VendorId!=0x10de) return test::kSkip;
    std::printf("Native DX11 / D3D12 device: %ls\n",desc.Description);
    ComPtr<ID3D11Device> native;
    ComPtr<ID3D11DeviceContext> ctx;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&native,nullptr,&ctx)));
    const wchar_t* cache=argc>1 ? argv[1] : L".";
    CreateDirectoryW(cache,nullptr);
    REQUIRE(na::dlss::InitOwn(d.device.Get(),cache));
    REQUIRE(Exercise(d,native.Get(),ctx.Get(),2408,1008,na::dlss::Quality::MaxQuality)==0);
    REQUIRE(Exercise(d,native.Get(),ctx.Get(),1720,720,na::dlss::Quality::MaxPerformance)==0);
    na::dlss::Shutdown();
    return 0;
}
